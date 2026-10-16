name			staterestore
version			@VERSION@-@REVISION@
architecture	x86_64
summary 		"Restore an earlier state of the system's packages"
description 	"StateRestore goes back to an earlier state of the system's packages without the boot menu. It lists the earlier states Haiku keeps, shows what restoring each one would change, swaps the packages (keeping what it replaces, so that a restore can be undone), deletes old states, and saves the system log for sending to someone. Restart afterwards for the old system to start."
packager		"ablyss <krisbeazley@gmail.com>"
vendor			"ablyss"
licenses {
	"MIT"
}
copyrights {
	"@YEAR@ ablyss"
}
provides {
	staterestore = @VERSION@-@REVISION@
	app:StateRestore = @VERSION@-@REVISION@
	cmd:StateRestore = @VERSION@-@REVISION@
}
requires {
	haiku
}
urls {
	"https://github.com/ablyssx74/StateRestore"
}
