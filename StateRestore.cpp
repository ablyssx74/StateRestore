/*
 * StateRestore: go back to an earlier state of the system's packages without the boot menu.
 *
 * Haiku keeps an old state of /boot/system as a folder (state_<date>_<time>) with a list of the packages that
 * were active, and the files of the packages the update removed. Restoring one is a matter of putting the packages
 * of that state in /boot/system/packages and taking the others out. Nothing is deleted: what is taken out is
 * kept, so that the restore can be undone.
 *
 * Command line (for testing):
 *   StateRestore --list | --plan <state> | --restore <state> [--dry-run] | --undo
 */

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <fs_info.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Path.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <ListItem.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextView.h>
#include <Volume.h>
#include <Window.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


#ifndef STATERESTORE_VERSION
#define STATERESTORE_VERSION "dev"
#endif

static const char* kAppSignature = "application/x-vnd.ablyss-StateRestore";
static const char* kPackagesDir = "/boot/system/packages";
static const char* kAdminDir = "/boot/system/packages/administrative";

typedef std::set<std::string> NameSet;


// ---------------------------------------------------------------------------------------------------------------
// The states
// ---------------------------------------------------------------------------------------------------------------

struct State {
	std::string	id;			// the folder's name, state_2026-10-15_17:52:34
	std::string	label;		// 2026-10-15 17:52:34
	NameSet		packages;	// what was active in it
	NameSet		files;		// the package files in its folder (the ones its change took out)
	int			changes;	// how many packages differ from the system now
	bool		restorable;	// all of what it needs is there, and it differs from now
	State() : changes(0), restorable(false) {}
};


// A line of an activation file is the file name of a package. A list that has anything else in it is damaged (the system's
// own file can be, after a shutdown in the middle of a write) and is not used.
static bool
LooksLikePackageFile(const char* line, size_t length)
{
	if (length < 6 || length > 250 || strcmp(line + length - 5, ".hpkg") != 0)
		return false;
	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)line[i];
		if (c < 0x20 || c >= 0x7f || c == '/')
			return false;
	}
	return true;
}


static bool
ReadList(const std::string& path, NameSet& out)
{
	FILE* file = fopen(path.c_str(), "r");
	if (file == NULL)
		return false;
	NameSet found;
	bool damaged = false;
	char line[2048];
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t length = strlen(line);
		while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = '\0';
		if (length == 0)
			continue;
		if (!LooksLikePackageFile(line, length)) {
			damaged = true;
			break;
		}
		found.insert(line);
	}
	fclose(file);
	if (damaged || found.empty())
		return false;
	out.insert(found.begin(), found.end());
	return true;
}


static bool
Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}


static std::string
HoldRoot()
{
	BPath path;
	if (find_directory(B_USER_VAR_DIRECTORY, &path, true) != B_OK)
		return "/boot/home/config/var/StateRestore";
	return std::string(path.Path()) + "/StateRestore";
}


static std::string
LabelOf(const std::string& id)
{
	std::string label = id.compare(0, 6, "state_") == 0 ? id.substr(6) : id;
	size_t underscore = label.find('_');
	if (underscore != std::string::npos)
		label[underscore] = ' ';
	return label;
}


static bool gCurrentListDamaged = false;	// the system's own list of active packages could not be read
static int gDamagedStates = 0;			// states whose list could not be read


static NameSet
CurrentPackages()
{
	NameSet current;
	gCurrentListDamaged = false;
	if (!ReadList(std::string(kAdminDir) + "/activated-packages", current)) {
		// the system's list is damaged: packagefs then loads every package in the packages folder, so that is the state
		gCurrentListDamaged = true;
		current.clear();
		DIR* dir = opendir(kPackagesDir);
		if (dir != NULL) {
			while (struct dirent* entry = readdir(dir)) {
				size_t length = strlen(entry->d_name);
				if (length > 5 && strcmp(entry->d_name + length - 5, ".hpkg") == 0)
					current.insert(entry->d_name);
			}
			closedir(dir);
		}
	}
	return current;
}


// What is in the packages folder and in the folders of earlier restores, read once for each loading of the states.
static NameSet gPackageFiles;
static std::map<std::string, std::string> gHeldFiles;

struct Plan;
static Plan MakePlan(const State& target, const std::vector<State>& states, const std::set<std::string>* excluded = NULL);


static void
ScanFolderForPackages(const std::string& folder, NameSet* names, std::map<std::string, std::string>* paths)
{
	DIR* dir = opendir(folder.c_str());
	if (dir == NULL)
		return;
	while (struct dirent* entry = readdir(dir)) {
		size_t length = strlen(entry->d_name);
		if (length < 5 || strcmp(entry->d_name + length - 5, ".hpkg") != 0)
			continue;
		if (names != NULL)
			names->insert(entry->d_name);
		if (paths != NULL && paths->count(entry->d_name) == 0)
			(*paths)[entry->d_name] = folder + "/" + entry->d_name;
	}
	closedir(dir);
}


// newest first
static std::vector<State>
LoadStates()
{
	std::vector<State> states;
	gDamagedStates = 0;
	DIR* dir = opendir(kAdminDir);
	if (dir == NULL)
		return states;
	while (struct dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "state_", 6) != 0)
			continue;
		State state;
		state.id = entry->d_name;
		state.label = LabelOf(state.id);
		if (!ReadList(std::string(kAdminDir) + "/" + state.id + "/activated-packages", state.packages)) {
			gDamagedStates++;
			continue;
		}
		ScanFolderForPackages(std::string(kAdminDir) + "/" + state.id, &state.files, NULL);
		states.push_back(state);
	}
	closedir(dir);
	std::sort(states.begin(), states.end(), [](const State& a, const State& b) { return a.id > b.id; });

	gPackageFiles.clear();
	gHeldFiles.clear();
	ScanFolderForPackages(kPackagesDir, &gPackageFiles, NULL);
	std::string held = HoldRoot() + "/held";
	if (DIR* heldDir = opendir(held.c_str())) {
		while (struct dirent* entry = readdir(heldDir)) {
			if (entry->d_name[0] != '.')
				ScanFolderForPackages(held + "/" + entry->d_name, NULL, &gHeldFiles);
		}
		closedir(heldDir);
	}
	return states;
}


// The name of a package without its version: haiku-r1~beta6_hrev60237-1-x86_64.hpkg is "haiku".
static std::string
BaseName(const std::string& file)
{
	size_t dash = file.find('-');
	return dash == std::string::npos ? file : file.substr(0, dash);
}


static std::string
VersionOf(const std::string& file)
{
	std::string base = BaseName(file);
	std::string rest = file.size() > base.size() + 1 ? file.substr(base.size() + 1) : file;
	size_t end = rest.rfind(".hpkg");
	if (end != std::string::npos)
		rest.resize(end);
	return rest;
}


// "hrev60237", from the haiku package of a state ("" when it has none)
static std::string
HaikuRevision(const NameSet& packages)
{
	for (NameSet::const_iterator it = packages.begin(); it != packages.end(); ++it) {
		if (BaseName(*it) != "haiku")
			continue;
		size_t at = it->find("hrev");
		if (at == std::string::npos)
			return "";
		size_t end = at + 4;
		while (end < it->size() && (*it)[end] >= '0' && (*it)[end] <= '9')
			end++;
		return it->substr(at, end - at);
	}
	return "";
}


// ---------------------------------------------------------------------------------------------------------------
// What a restore would do
// ---------------------------------------------------------------------------------------------------------------

struct Plan {
	std::vector<std::string>				remove;		// active now, not in the state
	std::vector<std::string>				add;		// in the state, not active now
	std::map<std::string, std::string>		source;		// where the file of each of those is
	std::vector<std::string>				missing;	// ... and the ones that are nowhere
	std::string								error;
};


// A package file of an earlier state: in the packages folder, in the folder of a state that removed it, or in the
// ones kept by an earlier restore. States in `excluded` are not looked in (they are about to be deleted).
static std::string
FindPackageFile(const std::string& name, const std::vector<State>& states, const std::set<std::string>* excluded)
{
	if (gPackageFiles.count(name) != 0)
		return std::string(kPackagesDir) + "/" + name;
	for (size_t i = 0; i < states.size(); i++) {
		if (states[i].files.count(name) != 0 && (excluded == NULL || excluded->count(states[i].id) == 0))
			return std::string(kAdminDir) + "/" + states[i].id + "/" + name;
	}
	std::map<std::string, std::string>::const_iterator held = gHeldFiles.find(name);
	if (held != gHeldFiles.end())
		return held->second;
	return "";
}


static Plan
MakePlan(const State& target, const std::vector<State>& states, const std::set<std::string>* excluded)
{
	Plan plan;
	NameSet current = CurrentPackages();
	if (current.empty()) {
		plan.error = "The list of active packages could not be read.";
		return plan;
	}
	for (NameSet::const_iterator it = current.begin(); it != current.end(); ++it) {
		if (target.packages.count(*it) == 0)
			plan.remove.push_back(*it);
	}
	for (NameSet::const_iterator it = target.packages.begin(); it != target.packages.end(); ++it) {
		if (current.count(*it) != 0)
			continue;
		plan.add.push_back(*it);
		std::string path = FindPackageFile(*it, states, excluded);
		if (path.empty())
			plan.missing.push_back(*it);
		else
			plan.source[*it] = path;
	}
	return plan;
}


// "haiku: r1~beta6_hrev60256-1-x86_64 -> r1~beta6_hrev60237-1-x86_64", for what is only a version change, and the rest
// as removed or restored.
static std::string
DescribePlan(const Plan& plan)
{
	std::string text;
	if (!plan.error.empty())
		return plan.error + "\n";

	std::map<std::string, std::string> removedByBase, addedByBase;
	for (size_t i = 0; i < plan.remove.size(); i++)
		removedByBase[BaseName(plan.remove[i])] = plan.remove[i];
	for (size_t i = 0; i < plan.add.size(); i++)
		addedByBase[BaseName(plan.add[i])] = plan.add[i];

	std::string changed, removed, restored;
	for (std::map<std::string, std::string>::const_iterator it = removedByBase.begin(); it != removedByBase.end(); ++it) {
		std::map<std::string, std::string>::const_iterator other = addedByBase.find(it->first);
		if (other != addedByBase.end())
			changed += "  " + it->first + ":  " + VersionOf(it->second) + "  ->  " + VersionOf(other->second) + "\n";
		else
			removed += "  " + it->second + "\n";
	}
	for (std::map<std::string, std::string>::const_iterator it = addedByBase.begin(); it != addedByBase.end(); ++it) {
		if (removedByBase.find(it->first) == removedByBase.end())
			restored += "  " + it->second + "\n";
	}

	if (changed.empty() && removed.empty() && restored.empty())
		return "This is the state the system is in now. Nothing would change.\n";
	if (!changed.empty())
		text += "Packages that change version:\n" + changed + "\n";
	if (!removed.empty())
		text += "Packages that are taken out (kept, so that the restore can be undone):\n" + removed + "\n";
	if (!restored.empty())
		text += "Packages that are put back:\n" + restored + "\n";
	if (!plan.missing.empty()) {
		text += "These files cannot be found, so this state cannot be restored:\n";
		for (size_t i = 0; i < plan.missing.size(); i++)
			text += "  " + plan.missing[i] + "\n";
	}
	return text;
}


static int
ChangeCount(const Plan& plan)
{
	std::set<std::string> bases;
	for (size_t i = 0; i < plan.remove.size(); i++)
		bases.insert(BaseName(plan.remove[i]));
	for (size_t i = 0; i < plan.add.size(); i++)
		bases.insert(BaseName(plan.add[i]));
	return (int)bases.size();
}


// The states, with what each needs and what differs from now worked out.
static std::vector<State>
LoadAllStates()
{
	std::vector<State> states = LoadStates();
	for (size_t i = 0; i < states.size(); i++) {
		Plan plan = MakePlan(states[i], states);
		states[i].changes = ChangeCount(plan);
		states[i].restorable = plan.error.empty() && plan.missing.empty() && states[i].changes > 0;
	}
	return states;
}


// ---------------------------------------------------------------------------------------------------------------
// Doing it
// ---------------------------------------------------------------------------------------------------------------

static bool
CopyFile(const std::string& from, const std::string& to, std::string& error)
{
	int in = open(from.c_str(), O_RDONLY);
	if (in < 0) {
		error = "Cannot read " + from + ": " + strerror(errno);
		return false;
	}
	int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		error = "Cannot write " + to + ": " + strerror(errno);
		close(in);
		return false;
	}
	static const size_t kBuffer = 1024 * 1024;
	char* buffer = (char*)malloc(kBuffer);
	off_t total = 0;
	bool ok = true;
	for (;;) {
		ssize_t got = read(in, buffer, kBuffer);
		if (got < 0) {
			error = "Reading " + from + " failed: " + strerror(errno);
			ok = false;
			break;
		}
		if (got == 0)
			break;
		ssize_t put = write(out, buffer, got);
		if (put != got) {
			error = "Writing " + to + " failed: " + strerror(errno);
			ok = false;
			break;
		}
		total += got;
	}
	free(buffer);
	close(in);
	if (ok && fsync(out) != 0 && errno != EINVAL && errno != ENOSYS) {
		error = "Writing " + to + " failed: " + strerror(errno);
		ok = false;
	}
	close(out);
	struct stat st;
	if (ok && (stat(from.c_str(), &st) != 0 || st.st_size != total)) {
		error = "The copy of " + from + " is not complete.";
		ok = false;
	}
	if (!ok)
		unlink(to.c_str());
	return ok;
}


static bool
WriteLines(const std::string& path, const std::vector<std::string>& lines)
{
	FILE* file = fopen(path.c_str(), "w");
	if (file == NULL)
		return false;
	for (size_t i = 0; i < lines.size(); i++)
		fprintf(file, "%s\n", lines[i].c_str());
	fclose(file);
	return true;
}


static std::string
Timestamp()
{
	char text[32];
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
	return text;
}


static std::string
LastRestoreFile()
{
	return HoldRoot() + "/last-restore";
}


// Returns an empty string when it went well, otherwise what went wrong.
static std::string
Restore(const State& target, const std::vector<State>& states, bool dryRun, std::string* report)
{
	Plan plan = MakePlan(target, states);
	if (report != NULL)
		*report = DescribePlan(plan);
	if (!plan.error.empty())
		return plan.error;
	if (!plan.missing.empty())
		return "Some package files of that state are gone, so it cannot be restored.";
	if (plan.remove.empty() && plan.add.empty())
		return "That is the state the system is in now.";
	if (dryRun)
		return "";

	// room for the copies
	off_t needed = 0;
	for (std::map<std::string, std::string>::const_iterator it = plan.source.begin(); it != plan.source.end(); ++it) {
		struct stat st;
		if (stat(it->second.c_str(), &st) == 0)
			needed += st.st_size;
	}
	BVolume volume(dev_for_path(kPackagesDir));
	if (volume.InitCheck() == B_OK && volume.FreeBytes() < needed + 64 * 1024 * 1024)
		return "There is not enough free space on the boot disk.";

	std::string holdDir = HoldRoot() + "/held/" + Timestamp();
	if (create_directory((HoldRoot() + "/held").c_str(), 0755) != B_OK || create_directory(holdDir.c_str(), 0755) != B_OK)
		return "Cannot make the folder for the packages that are taken out.";

	// 1. the packages to put back are copied next to the others under a name the system ignores
	std::string error;
	std::vector<std::string> staged;
	for (size_t i = 0; i < plan.add.size(); i++) {
		std::string staging = std::string(kPackagesDir) + "/" + plan.add[i] + ".stage";
		if (!CopyFile(plan.source[plan.add[i]], staging, error)) {
			for (size_t j = 0; j < staged.size(); j++)
				unlink(staged[j].c_str());
			return error;
		}
		staged.push_back(staging);
	}

	// 2. the swap, quickly, so that the system is without a package for as short a time as it can be
	std::vector<std::string> taken;
	for (size_t i = 0; i < plan.remove.size(); i++) {
		std::string from = std::string(kPackagesDir) + "/" + plan.remove[i];
		if (rename(from.c_str(), (holdDir + "/" + plan.remove[i]).c_str()) != 0) {
			error = "Cannot take out " + plan.remove[i] + ": " + strerror(errno);
			// put back what was taken out, and clean up
			for (size_t j = 0; j < taken.size(); j++)
				rename((holdDir + "/" + taken[j]).c_str(), (std::string(kPackagesDir) + "/" + taken[j]).c_str());
			for (size_t j = 0; j < staged.size(); j++)
				unlink(staged[j].c_str());
			return error;
		}
		taken.push_back(plan.remove[i]);
	}
	for (size_t i = 0; i < plan.add.size(); i++) {
		std::string to = std::string(kPackagesDir) + "/" + plan.add[i];
		if (rename(staged[i].c_str(), to.c_str()) != 0)
			error = "Cannot put in " + plan.add[i] + ": " + strerror(errno);
	}

	WriteLines(holdDir + "/removed.txt", plan.remove);
	WriteLines(holdDir + "/added.txt", plan.add);
	WriteLines(LastRestoreFile(), std::vector<std::string>(1, holdDir));
	if (report != NULL)
		*report = DescribePlan(plan);
	return error;
}


static std::string
Undo(std::string* report)
{
	NameSet last;
	if (!ReadList(LastRestoreFile(), last) || last.empty())
		return "There is no restore to undo.";
	std::string holdDir = *last.begin();
	NameSet removed, added;
	if (!ReadList(holdDir + "/removed.txt", removed) || !ReadList(holdDir + "/added.txt", added))
		return "The record of the last restore is not complete.";

	for (NameSet::const_iterator it = removed.begin(); it != removed.end(); ++it) {
		if (!Exists(holdDir + "/" + *it))
			return "The kept package " + *it + " is gone, so the restore cannot be undone.";
	}

	// the kept packages are put next to the others, then the swap back
	std::string undoneDir = holdDir + "/undone";
	create_directory(undoneDir.c_str(), 0755);
	for (NameSet::const_iterator it = removed.begin(); it != removed.end(); ++it)
		rename((holdDir + "/" + *it).c_str(), (std::string(kPackagesDir) + "/" + *it + ".stage").c_str());
	for (NameSet::const_iterator it = added.begin(); it != added.end(); ++it)
		rename((std::string(kPackagesDir) + "/" + *it).c_str(), (undoneDir + "/" + *it).c_str());
	for (NameSet::const_iterator it = removed.begin(); it != removed.end(); ++it) {
		std::string to = std::string(kPackagesDir) + "/" + *it;
		rename((to + ".stage").c_str(), to.c_str());
	}
	unlink(LastRestoreFile().c_str());
	if (report != NULL)
		*report = "The restore of " + holdDir + " is undone.";
	return "";
}


// Copies the system log and a few facts to a folder on the Desktop, for sending to someone.
static std::string
SaveSyslog(std::string* where)
{
	BPath desktop;
	if (find_directory(B_DESKTOP_DIRECTORY, &desktop, true) != B_OK)
		return "Cannot find the Desktop.";
	std::string folder = std::string(desktop.Path()) + "/syslog-" + Timestamp();
	if (create_directory(folder.c_str(), 0755) != B_OK)
		return "Cannot make " + folder;
	std::string error;
	const char* logs[] = { "/var/log/syslog", "/var/log/syslog.old", "/var/log/syslog.1" };
	for (size_t i = 0; i < sizeof(logs) / sizeof(logs[0]); i++) {
		if (Exists(logs[i]))
			CopyFile(logs[i], folder + "/" + (strrchr(logs[i], '/') + 1) + ".txt", error);
	}
	std::string command = "(uname -a; echo; listusb; echo; listdev) > '" + folder + "/system-info.txt' 2>&1";
	system(command.c_str());
	CopyFile(std::string(kAdminDir) + "/activated-packages", folder + "/activated-packages.txt", error);
	if (where != NULL)
		*where = folder;
	return "";
}


// The size of the files in the folder of a state.
static off_t
StateSize(const std::string& id)
{
	off_t total = 0;
	std::string folder = std::string(kAdminDir) + "/" + id;
	if (DIR* dir = opendir(folder.c_str())) {
		while (struct dirent* entry = readdir(dir)) {
			struct stat st;
			if (entry->d_name[0] != '.' && stat((folder + "/" + entry->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
				total += st.st_size;
		}
		closedir(dir);
	}
	return total;
}


// Deletes the folder of an earlier state. Only a folder named state_... in the administrative folder is touched.
static std::string
DeleteState(const std::string& id)
{
	if (id.compare(0, 6, "state_") != 0 || id.find('/') != std::string::npos || id.find("..") != std::string::npos)
		return "Not the name of a state: " + id;
	std::string folder = std::string(kAdminDir) + "/" + id;
	DIR* dir = opendir(folder.c_str());
	if (dir == NULL)
		return "Cannot open " + id + ": " + strerror(errno);
	std::vector<std::string> files;
	while (struct dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			files.push_back(entry->d_name);
	}
	closedir(dir);
	for (size_t i = 0; i < files.size(); i++) {
		if (unlink((folder + "/" + files[i]).c_str()) != 0)
			return "Cannot delete " + files[i] + " of " + id + ": " + strerror(errno);
	}
	if (rmdir(folder.c_str()) != 0)
		return "Cannot remove the folder of " + id + ": " + strerror(errno);
	return "";
}


// How many of the states that can be restored now could not be restored any more without the given ones: the file of
// a package is in the folder of the state that took it out, and an older state can need it too.
static int
CountBroken(const std::vector<State>& states, const std::set<std::string>& deleting)
{
	int broken = 0;
	for (size_t i = 0; i < states.size(); i++) {
		if (!states[i].restorable || deleting.count(states[i].id) != 0)
			continue;
		Plan after = MakePlan(states[i], states, &deleting);
		if (!after.missing.empty())
			broken++;
	}
	return broken;
}


// The states that cannot be restored, or that are the same as the system now, and can be deleted without taking a file
// that a state which can be restored needs. `kept` counts the ones left out for that reason.
static std::vector<std::string>
CleanupSet(const std::vector<State>& states, int* kept)
{
	std::set<std::string> deleting;
	int keptCount = 0;
	for (size_t i = 0; i < states.size(); i++) {
		if (states[i].restorable)
			continue;
		if (states[i].files.empty()) {
			deleting.insert(states[i].id);
			continue;
		}
		// it has package files: only when no restorable state needs them
		std::set<std::string> trial = deleting;
		trial.insert(states[i].id);
		if (CountBroken(states, trial) == 0)
			deleting = trial;
		else
			keptCount++;
	}
	if (kept != NULL)
		*kept = keptCount;
	return std::vector<std::string>(deleting.begin(), deleting.end());
}


static std::string
SizeText(off_t bytes)
{
	char text[32];
	if (bytes >= 1024 * 1024 * 1024)
		snprintf(text, sizeof(text), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
	else
		snprintf(text, sizeof(text), "%.0f MB", bytes / (1024.0 * 1024.0));
	return text;
}


// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

enum {
	kMsgSelected	= 'sel!',
	kMsgRestore		= 'rst!',
	kMsgDelete		= 'del!',
	kMsgCleanup		= 'cln!',
	kMsgUndo		= 'und!',
	kMsgRestart		= 'rbt!',
	kMsgRefresh		= 'rfr!',
	kMsgWorkDone	= 'wrk!'
};


struct Job {
	BMessenger		window;
	std::string		kind;		// "restore" or "undo"
	State			target;
	std::vector<State> states;
	std::vector<std::string> ids;	// the states to delete
};


static int32
RunJob(void* data)
{
	Job* job = (Job*)data;
	std::string report;
	std::string error;
	if (job->kind == "restore")
		error = Restore(job->target, job->states, false, &report);
	else if (job->kind == "delete") {
		off_t freed = 0;
		int deleted = 0;
		for (size_t i = 0; i < job->ids.size() && error.empty(); i++) {
			freed += StateSize(job->ids[i]);
			error = DeleteState(job->ids[i]);
			if (error.empty())
				deleted++;
		}
		char text[96];
		snprintf(text, sizeof(text), "Deleted %d %s, %s freed.", deleted, deleted == 1 ? "state" : "states",
			SizeText(freed).c_str());
		report = text;
	} else
		error = Undo(&report);
	BMessage done(kMsgWorkDone);
	done.AddString("kind", job->kind.c_str());
	done.AddString("error", error.c_str());
	done.AddString("report", report.c_str());
	job->window.SendMessage(&done);
	delete job;
	return 0;
}


// A row of the list: the current system (bold, on a tinted background), a heading, or an earlier state that can be
// restored. The columns are the date, the Haiku revision and what differs from now.
class StateItem : public BListItem {
public:
	enum Kind { kCurrent, kHeading, kState };

	StateItem(Kind kind, const State& state, const BString& date, const BString& revision, const BString& detail,
		const BString& tag)
		:
		BListItem(),
		fKind(kind),
		fState(state),
		fDate(date),
		fRevision(revision),
		fDetail(detail),
		fTag(tag)
	{
		SetEnabled(kind != kHeading);
	}

	Kind GetKind() const { return fKind; }
	const State& GetState() const { return fState; }

	void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		SetHeight(ceilf(height.ascent + height.descent + height.leading) + 8);
		fAscent = height.ascent;
	}

	void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
		rgb_color text = ui_color(B_LIST_ITEM_TEXT_COLOR);
		if (fKind == kCurrent)
			background = tint_color(ui_color(B_CONTROL_HIGHLIGHT_COLOR), 1.6f);
		if (IsSelected()) {
			background = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
			text = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
		} else if (fKind == kCurrent) {
			text = ui_color(B_LIST_ITEM_TEXT_COLOR);
		}
		owner->SetHighColor(background);
		owner->FillRect(frame);

		BFont font;
		owner->GetFont(&font);
		BFont bold(font);
		bold.SetFace(B_BOLD_FACE);
		float baseline = frame.top + 4 + fAscent;

		if (fKind == kHeading) {
			owner->SetFont(&bold);
			owner->SetHighColor(tint_color(ui_color(B_LIST_ITEM_TEXT_COLOR), B_LIGHTEN_1_TINT));
			owner->DrawString(fDate.String(), BPoint(frame.left + 6, baseline));
			owner->SetFont(&font);
			return;
		}

		// the tag, the date, the Haiku revision and what differs, in columns
		float x = frame.left + 6;
		if (fTag.Length() > 0) {
			owner->SetFont(&bold);
			owner->SetHighColor(IsSelected() ? text : tint_color(ui_color(B_CONTROL_HIGHLIGHT_COLOR), 0.7f));
			owner->DrawString(fTag.String(), BPoint(x, baseline));
		}
		x += bold.StringWidth("PREVIOUS") + 16;
		owner->SetHighColor(text);
		owner->SetFont(fKind == kCurrent ? &bold : &font);
		owner->DrawString(fDate.String(), BPoint(x, baseline));
		x += font.StringWidth("2026-10-16 13:25:27") + 20;
		owner->DrawString(fRevision.String(), BPoint(x, baseline));
		x += font.StringWidth("hrev00000") + 20;
		owner->DrawString(fDetail.String(), BPoint(x, baseline));
		owner->SetFont(&font);
	}

private:
	Kind	fKind;
	State	fState;
	BString	fDate, fRevision, fDetail, fTag;
	float	fAscent;
};


class StateWindow : public BWindow {
public:
	StateWindow()
		:
		BWindow(BRect(100, 100, 760, 520), "State Restore " STATERESTORE_VERSION, B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
		fBusy(false),
		fRefreshRunner(NULL)
	{
		BStringView* intro = new BStringView("intro",
			"Go back to an earlier state of the system's packages, without the boot menu.");
		BStringView* hint = new BStringView("hint",
			"A restore keeps what it replaces, and Undo puts it back. Restart afterwards for the old system to start.");
		hint->SetHighColor(tint_color(ui_color(B_PANEL_TEXT_COLOR), B_LIGHTEN_1_TINT));

		fList = new BListView("states", B_MULTIPLE_SELECTION_LIST);
		fList->SetSelectionMessage(new BMessage(kMsgSelected));
		BScrollView* listScroll = new BScrollView("listScroll", fList, 0, false, true);

		fPreview = new BTextView("preview");
		fPreview->MakeEditable(false);
		fPreview->SetWordWrap(true);
		BScrollView* previewScroll = new BScrollView("previewScroll", fPreview, 0, false, true);

		fRestore = new BButton("restore", "Restore selected state" B_UTF8_ELLIPSIS, new BMessage(kMsgRestore));
		fUndo = new BButton("undo", "Undo last restore", new BMessage(kMsgUndo));
		fDelete = new BButton("delete", "Delete selected" B_UTF8_ELLIPSIS, new BMessage(kMsgDelete));
		fCleanup = new BButton("cleanup", "Delete unrestorable" B_UTF8_ELLIPSIS, new BMessage(kMsgCleanup));
		fRestart = new BButton("restart", "Restart" B_UTF8_ELLIPSIS, new BMessage(kMsgRestart));
		fStatus = new BStringView("status", "");

		listScroll->SetExplicitMinSize(BSize(540, 200));
		listScroll->SetExplicitPreferredSize(BSize(560, 320));
		previewScroll->SetExplicitMinSize(BSize(260, 200));
		previewScroll->SetExplicitPreferredSize(BSize(320, 320));
		BSplitView* split = new BSplitView(B_HORIZONTAL);
		split->AddChild(listScroll, 0.42f);
		split->AddChild(previewScroll, 0.58f);

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_INSETS)
			.Add(intro)
			.Add(hint)
			.Add(split)
			.AddGroup(B_HORIZONTAL)
				.Add(fRestore)
				.Add(fUndo)
				.Add(fDelete)
				.Add(fCleanup)
				.AddGlue()
				.Add(fRestart)
			.End()
			.Add(fStatus);

		Reload();
	}

	bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSelected:
				ShowPreview();
				break;
			case kMsgRestore:
				StartRestore();
				break;
			case kMsgUndo:
				StartUndo();
				break;
			case kMsgDelete:
				StartDelete();
				break;
			case kMsgCleanup:
				StartCleanup();
				break;
			case kMsgRestart:
			{
				BAlert* alert = new BAlert("restart", "Restart the computer now?", "Not now", "Restart", NULL,
					B_WIDTH_AS_USUAL, B_WARNING_ALERT);
				alert->SetShortcut(0, B_ESCAPE);
				if (alert->Go() == 1)
					system("shutdown -r &");
				break;
			}
			case kMsgRefresh:
				Reload();
				break;
			case kMsgWorkDone:
				WorkDone(message);
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void SetStatus(const char* text)
	{
		fStatus->SetText(text);
	}

	void Reload()
	{
		fStates = LoadAllStates();
		fList->MakeEmpty();

		NameSet current = CurrentPackages();
		BString since;
		struct stat st;
		if (stat((std::string(kAdminDir) + "/activated-packages").c_str(), &st) == 0) {
			char text[32];
			struct tm local;
			localtime_r(&st.st_mtime, &local);
			strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
			since = text;
		}
		BString revision(HaikuRevision(current).c_str());
		State nowState;
		nowState.id = "";
		nowState.packages = current;
		BString detail;
		detail << (int)current.size() << " packages";
		fList->AddItem(new StateItem(StateItem::kCurrent, nowState, since, revision, detail, "CURRENT"));

		// only the states that can be restored: they have what the system has not, and all of their files are there
		int added = 0;
		for (size_t i = 0; i < fStates.size(); i++) {
			if (!fStates[i].restorable)
				continue;
			int changes = fStates[i].changes;
			if (added == 0) {
				fList->AddItem(new StateItem(StateItem::kHeading, State(), "Earlier states you can restore", "", "",
					""));
			}
			BString text;
			text << changes << (changes == 1 ? " package differs" : " packages differ");
			BString rev(HaikuRevision(fStates[i].packages).c_str());
			// a state with another Haiku than the one running is marked as older or newer
			BString tag(added == 0 ? "PREVIOUS" : "");
			if (rev.Length() > 4 && revision.Length() > 4 && rev != revision)
				tag = atol(rev.String() + 4) < atol(revision.String() + 4) ? "OLDER" : "NEWER";
			fList->AddItem(new StateItem(StateItem::kState, fStates[i], fStates[i].label.c_str(), rev, text, tag));
			added++;
		}
		fPreview->SetText(added > 0 ? "Pick a state on the left to see what a restore would change."
			: "There is no earlier state that can be restored.");
		BString note;
		if (gCurrentListDamaged) {
			note << "The system's list of active packages is damaged (a shutdown in the middle of a write can do that); "
				"the packages folder is used instead. ";
		}
		if (gDamagedStates > 0) {
			note << gDamagedStates << (gDamagedStates == 1 ? " state with a damaged list is" : " states with damaged lists are")
				<< " left out.";
		}
		if (note.Length() > 0 && strlen(fStatus->Text()) == 0)
			SetStatus(note.String());
		fRestore->SetEnabled(false);
		UpdateButtons();
	}

	void UpdateButtons()
	{
		std::vector<const State*> picked = SelectedStates();
		fRestore->SetEnabled(!fBusy && picked.size() == 1 && SelectedPlanCanRun());
		fUndo->SetEnabled(!fBusy && Exists(LastRestoreFile()));
		fDelete->SetEnabled(!fBusy && !picked.empty());
		fCleanup->SetEnabled(!fBusy);
	}

	// the earlier states that are selected (not the current system, not the heading)
	std::vector<const State*> SelectedStates() const
	{
		std::vector<const State*> picked;
		for (int32 i = 0, index; (index = fList->CurrentSelection(i)) >= 0; i++) {
			StateItem* item = dynamic_cast<StateItem*>(fList->ItemAt(index));
			if (item != NULL && item->GetKind() == StateItem::kState)
				picked.push_back(&item->GetState());
		}
		return picked;
	}

	// the one earlier state that is selected, or none when there are none or several
	const State* Selected() const
	{
		std::vector<const State*> picked = SelectedStates();
		return picked.size() == 1 ? picked[0] : NULL;
	}

	bool SelectedPlanCanRun() const
	{
		const State* state = Selected();
		if (state == NULL)
			return false;
		Plan plan = MakePlan(*state, fStates);
		return plan.error.empty() && plan.missing.empty() && (!plan.remove.empty() || !plan.add.empty());
	}

	void ShowPreview()
	{
		StateItem* picked = dynamic_cast<StateItem*>(fList->ItemAt(fList->CurrentSelection()));
		if (picked != NULL && picked->GetKind() == StateItem::kCurrent) {
			fPreview->SetText("This is the system you are running now.\n\nPick one of the earlier states to see "
				"what going back to it would change.");
			UpdateButtons();
			return;
		}
		std::vector<const State*> many = SelectedStates();
		if (many.size() > 1) {
			std::set<std::string> ids;
			off_t size = 0;
			for (size_t i = 0; i < many.size(); i++) {
				ids.insert(many[i]->id);
				size += StateSize(many[i]->id);
			}
			char text[200];
			snprintf(text, sizeof(text), "%d states are selected.\n\nDeleting them would free about %s.\n",
				(int)many.size(), SizeText(size).c_str());
			std::string summary = text;
			int broken = CountBroken(fStates, ids);
			if (broken > 0) {
				snprintf(text, sizeof(text), "\n%d other %s could not be restored any more.\n", broken,
					broken == 1 ? "state" : "states");
				summary += text;
			}
			fPreview->SetText(summary.c_str());
			UpdateButtons();
			return;
		}
		const State* state = Selected();
		if (state == NULL) {
			UpdateButtons();
			return;
		}
		Plan plan = MakePlan(*state, fStates);
		std::string text = "State of " + state->label + "\n\n" + DescribePlan(plan);
		fPreview->SetText(text.c_str());
		UpdateButtons();
	}

	void StartRestore()
	{
		const State* state = Selected();
		if (state == NULL || fBusy)
			return;
		BString question("Go back to the state of ");
		question << state->label.c_str() << "?\n\nThe packages that differ are swapped. They are kept, and Undo puts "
			"them back. You restart afterwards.";
		BAlert* alert = new BAlert("confirm", question.String(), "Cancel", "Restore", NULL, B_WIDTH_AS_USUAL,
			B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			return;
		Start("restore", *state);
	}

	void StartDelete()
	{
		if (fBusy)
			return;
		std::vector<const State*> picked = SelectedStates();
		if (picked.empty())
			return;
		std::set<std::string> ids;
		off_t size = 0;
		for (size_t i = 0; i < picked.size(); i++) {
			ids.insert(picked[i]->id);
			size += StateSize(picked[i]->id);
		}
		int broken = CountBroken(fStates, ids);

		BString question;
		question << "Delete " << (int)picked.size() << (picked.size() == 1 ? " state?" : " states?")
			<< "\n\nThis frees about " << SizeText(size).c_str() << ". They cannot be restored afterwards, and this "
			"cannot be undone.";
		if (broken > 0) {
			question << "\n\n" << broken << " other " << (broken == 1 ? "state" : "states")
				<< " that can be restored now would not be restorable any more, because they need files kept in "
				"the states you delete.";
		}
		question << "\n\nThe system you are running now is not touched.";
		BAlert* alert = new BAlert("confirm", question.String(), "Cancel", "Delete", NULL, B_WIDTH_AS_USUAL,
			B_STOP_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			return;

		StartDeleteJob(std::vector<std::string>(ids.begin(), ids.end()));
	}

	void StartCleanup()
	{
		if (fBusy)
			return;
		int kept = 0;
		std::vector<std::string> ids = CleanupSet(fStates, &kept);
		if (ids.empty()) {
			BAlert* none = new BAlert("none", "There are no states to clean up.", "OK", NULL, NULL, B_WIDTH_AS_USUAL,
				B_INFO_ALERT);
			none->Go();
			return;
		}
		off_t size = 0;
		for (size_t i = 0; i < ids.size(); i++)
			size += StateSize(ids[i]);

		BString question;
		question << "Delete " << (int)ids.size() << " old " << (ids.size() == 1 ? "state" : "states")
			<< " that cannot be restored?\n\nThey are the states with package files missing, and the ones that are the "
			"same as the system now. This frees about " << SizeText(size).c_str()
			<< ". It cannot be undone.";
		if (kept > 0) {
			question << "\n\n" << kept << (kept == 1 ? " is" : " are") << " kept, because a state that can be "
				"restored needs files in " << (kept == 1 ? "it" : "them") << ".";
		}
		question << "\n\nThe states that can be restored, and the system you are running now, are not touched.";
		BAlert* alert = new BAlert("confirm", question.String(), "Cancel", "Delete", NULL, B_WIDTH_AS_USUAL,
			B_STOP_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			return;
		StartDeleteJob(ids);
	}

	void StartDeleteJob(const std::vector<std::string>& ids)
	{
		Job* job = new Job;
		job->window = BMessenger(this);
		job->kind = "delete";
		job->ids = ids;
		fBusy = true;
		UpdateButtons();
		SetStatus("Deleting...");
		thread_id thread = spawn_thread(RunJob, "state delete", B_NORMAL_PRIORITY, job);
		resume_thread(thread);
	}

	void StartUndo()
	{
		if (fBusy)
			return;
		BAlert* alert = new BAlert("confirm", "Undo the last restore?\n\nThe packages that were taken out come back.",
			"Cancel", "Undo", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			return;
		Start("undo", State());
	}

	void Start(const char* kind, const State& state)
	{
		Job* job = new Job;
		job->window = BMessenger(this);
		job->kind = kind;
		job->target = state;
		job->states = fStates;
		fBusy = true;
		UpdateButtons();
		SetStatus(strcmp(kind, "restore") == 0 ? "Swapping the packages..." : "Putting the packages back...");
		thread_id thread = spawn_thread(RunJob, "state restore", B_NORMAL_PRIORITY, job);
		resume_thread(thread);
	}

	void WorkDone(BMessage* message)
	{
		fBusy = false;
		BString error = message->GetString("error", "");
		BString report = message->GetString("report", "");
		BString kind = message->GetString("kind", "");
		if (error.Length() > 0) {
			SetStatus(error.String());
			BAlert* alert = new BAlert("error", error.String(), "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
			alert->Go();
		} else {
			if (kind == "delete")
				SetStatus(report.String());
			else
				SetStatus(kind == "restore"
					? "Done. The system has the packages of that state; restart for it to start."
					: "Done. The packages of before the restore are back; restart for it to take effect.");
		}
		// the system takes a few seconds to write its record of the change
		BMessage refresh(kMsgRefresh);
		delete fRefreshRunner;
		fRefreshRunner = new BMessageRunner(this, &refresh, 4000000, 1);
		Reload();
		if (report.Length() > 0 && kind != "delete")
			fPreview->SetText(report.String());
	}

	BListView*			fList;
	BTextView*			fPreview;
	BButton*			fRestore;
	BButton*			fUndo;
	BButton*			fDelete;
	BButton*			fCleanup;
	BButton*			fRestart;
	BStringView*		fStatus;
	std::vector<State>	fStates;
	bool				fBusy;
	BMessageRunner*		fRefreshRunner;
};


class StateApp : public BApplication {
public:
	StateApp()
		:
		BApplication(kAppSignature)
	{
	}

	void ReadyToRun()
	{
		(new StateWindow())->Show();
	}
};


// ---------------------------------------------------------------------------------------------------------------

static const State*
FindState(const std::vector<State>& states, const char* id)
{
	std::string wanted = id;
	if (wanted.compare(0, 6, "state_") != 0)
		wanted = "state_" + wanted;
	for (size_t i = 0; i < states.size(); i++) {
		if (states[i].id == wanted)
			return &states[i];
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	if (argc >= 2) {
		std::vector<State> states = LoadAllStates();
		std::string command = argv[1];
		if (command == "--list") {
			if (gCurrentListDamaged)
				fprintf(stderr, "The system's list of active packages is damaged; the packages folder is used instead.\n");
			if (gDamagedStates > 0)
				fprintf(stderr, "%d states with a damaged list are left out.\n", gDamagedStates);
			for (size_t i = 0; i < states.size(); i++) {
				Plan plan = MakePlan(states[i], states);
				printf("%s  %d differ%s\n", states[i].id.c_str(), ChangeCount(plan), plan.missing.empty() ? "" : "  (files missing)");
			}
			return 0;
		}
		if ((command == "--plan" || command == "--restore") && argc >= 3) {
			const State* state = FindState(states, argv[2]);
			if (state == NULL) {
				fprintf(stderr, "No such state.\n");
				return 1;
			}
			bool dryRun = command == "--plan" || (argc >= 4 && strcmp(argv[3], "--dry-run") == 0);
			std::string report;
			std::string error = Restore(*state, states, dryRun, &report);
			printf("%s", report.c_str());
			if (!error.empty()) {
				fprintf(stderr, "%s\n", error.c_str());
				return 1;
			}
			return 0;
		}
		if (command == "--undo") {
			std::string report;
			std::string error = Undo(&report);
			printf("%s\n", report.c_str());
			if (!error.empty()) {
				fprintf(stderr, "%s\n", error.c_str());
				return 1;
			}
			return 0;
		}
		if (command == "--delete" && argc >= 3) {
			bool dryRun = strcmp(argv[argc - 1], "--dry-run") == 0;
			std::set<std::string> ids;
			off_t size = 0;
			for (int i = 2; i < argc; i++) {
				if (strcmp(argv[i], "--dry-run") == 0)
					continue;
				const State* state = FindState(states, argv[i]);
				if (state == NULL) {
					fprintf(stderr, "No such state: %s\n", argv[i]);
					return 1;
				}
				ids.insert(state->id);
				size += StateSize(state->id);
			}
			printf("%d states, %s, %d other states would not be restorable any more\n", (int)ids.size(),
				SizeText(size).c_str(), CountBroken(states, ids));
			if (dryRun)
				return 0;
			for (std::set<std::string>::const_iterator it = ids.begin(); it != ids.end(); ++it) {
				std::string error = DeleteState(*it);
				if (!error.empty()) {
					fprintf(stderr, "%s\n", error.c_str());
					return 1;
				}
				printf("deleted %s\n", it->c_str());
			}
			return 0;
		}
		if (command == "--cleanup") {
			int kept = 0;
			std::vector<std::string> ids = CleanupSet(states, &kept);
			off_t size = 0;
			for (size_t i = 0; i < ids.size(); i++)
				size += StateSize(ids[i]);
			printf("%d states, %s, %d kept because restorable states need their files\n", (int)ids.size(),
				SizeText(size).c_str(), kept);
			if (argc >= 3 && strcmp(argv[2], "--dry-run") == 0)
				return 0;
			for (size_t i = 0; i < ids.size(); i++) {
				std::string error = DeleteState(ids[i]);
				if (!error.empty()) {
					fprintf(stderr, "%s\n", error.c_str());
					return 1;
				}
			}
			printf("deleted %d states\n", (int)ids.size());
			return 0;
		}
		if (command == "--syslog") {
			std::string where;
			std::string error = SaveSyslog(&where);
			printf("%s\n", error.empty() ? where.c_str() : error.c_str());
			return error.empty() ? 0 : 1;
		}
		fprintf(stderr, "StateRestore [--list | --plan <state> | --restore <state> [--dry-run] | --undo | --delete <state>... [--dry-run] | --cleanup [--dry-run] | --syslog]\n");
		return 1;
	}

	StateApp app;
	app.Run();
	return 0;
}
