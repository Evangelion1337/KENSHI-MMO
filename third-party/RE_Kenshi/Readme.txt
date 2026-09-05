Installation:
Extract anywhere, run the installer (RE_Kenshi_vx.x.x.exe) and follow instructions.
After installation, RE_Kenshi should load by default when running Kenshi. You can also use the "RE_Kenshi" shortcuts which open about a second faster than running Kenshi normally due to skipping some initialization.

Uninstallation:
Run the installer (RE_Kenshi_vx.x.x.exe), select your Kenshi install dir, and hit "uninstall"
If this fails, you can manually disable the mod by removing the line "Plugin=RE_Kenshi" from "Plugins_x64.cfg" in your Kenshi install folder.

Notes:
RE_Kenshi is based on Kenshi 1.0.65. If installing to 1.0.68, a downgraded version of Kenshi will automatically be created during RE_Kenshi's installation.
If you want to use Kenshi 1.0.68 (with RE_Kenshi disabled) add the flag "--norekenshi" to your Kenshi launch arguments.
The installer should successfully update from any previous version
The mod uses up to ~200MB of disk space, make sure you have some free
Toggling heightmap settings at the main menu should be safe.
Toggling heightmap settings in-game may cause a crash.
Having the "DEBUG LOG" tab open will probably hurt performance, I recommend staying on the "SETTINGS" tab or closing the mod menu.
To reopen the mod menu: (at main menu or esc menu) -> OPTIONS -> MODS -> RE_KENSHI SETTINGS
For information on modding extensions, consult the wiki: https://github.com/BFrizzleFoShizzle/RE_Kenshi/wiki

If you have crashes or experience bugs, either use the in-game bug/crash reporter (accessible via the RE_Kenshi settings menu) or message me on:
Discord: BFrizzleFoShizzle

Compatibility:
1.0.65 Steam
1.0.68 Steam
1.0.65 GOG
1.0.68 GOG

Changelog:
0.3.5
KenshiLib update
Performance improvements, reduced load time to main menu by 1-2 seconds
Bugfix for plugin load errors always printing as "no error"
Minor Linux installer improvements
0.3.4
KenshiLib update
Fixed/improved some error messages
Bugfix for freecam keybind not showing up in control options for non-english languages
0.3.3
Fix for crash handler triggering on exit
Fix for emergency save triggering while at main menu
Installer fixes to properly handle unicode paths
Reduced crash dump report frequency (doesn't affect emergency save system)
0.3.2-v2
Fixed an installer issue
0.3.2
KenshiLib update
Added freecam
Bugfix for DirectX DLL proxying/hijacking not working (fixes ReShade, etc)
Bugfix for installer not creating shortcuts
Bugfix for plugin dependencies potentially not loading from plugin directories
Improved crash handler/emergency save reliability
0.3.1
Added French translation (Credit to Gearpunk)
Added setting to skip intro splash screens
KenshiLib update
Modified the startup procedure to make RE_Kenshi launch by default again (should also fix some Linux issues)
Fixed some PhysX cache crashes
Fixed a rare crash in RE_Kenshi's UI initialization
0.3.0
Added mod plugin system
Possibly fixed some installer issues, particularly around missing plugins_x64.cfg backups
0.2.18
Added new compression codecs
Added mipmap load skipping optimization
Added PhysX collider cache
Bugfix for crash in version checker
Bugfix for rare crash in heightmap code
Bugfix for texture settings not applying to some BC7 textures (fix applied only when using new mipmap loader)
Changed internal RE_Kenshi heightmap paths to be more consistent with vanilla (should be backwards-compatible with existing mods)
Multiple localization bugfixes
0.2.17
Bugfix for crash handler sometimes getting triggered when exiting normally
Bugfix for emergency save system getting triggered during crashes at the main menu
Possible bugfix for crash dump reporter not packaging files correctly
0.2.16
Bugfix for RNG fix crashes
Bugfix for crashes at the edge of the map
Crash handler improvements
0.2.15
Removed support for Steam + GOG 1.0.64
Fixed building part + foliage orientation issues caused by the RNG fix
Fixed foliage at map edges having incorrect position under some settings
Fixed a rare crash at the main menu
Improved error handling during initialization
Stopped Kenshi from scrolling to the top of the mod list whenever a mod is toggled/rearranged
Packaged installer in self-extracting archive (should fix Linux installer issues)
0.2.14
Added support for GOG 1.0.68
Fixed a bug that caused crashes on the edge of the map
0.2.13
Added support for Steam 1.0.68
Removed support for Steam 1.0.55
Fixed an uncommon bug that would crash the game before the main menu
Added a hidden setting for bypassing the hash check in RE_Kenshi's version checker
0.2.12
Added support for Steam 1.0.65
Added support for GOG 1.0.65
Removed support for GOG 1.0.59
Fixed some crashes in the fast heightmap code caused by non-standard TIFF files
0.2.11
Added support for GOG 1.0.64
Fixed a bug that would sometimes stop the "unsupported version" text from appearing on unsupported Kenshi versions
Added Japanese translation (Credit to Chigasane)
Updated German translation (Credit to Boron)
Updated Russian translation (Credit to Hack)
Refactored RE_Kenshi settings UI to make it less error-prone and be more in line with Kenshi's own UI design
Also improved handling of font + UI scaling
Fixed a logging bug causing "memory protection error" spam in the logs
Added fast uncompressed heightmap loader ("compressed" is faster on HDDs and low-end SSDs, "fast uncompressed" should be faster on high-end SSDs and has the added benefit of working directly on the vanilla TIF file)
(both heightmap implementations should beat the vanilla implementation on virtually all hardware configurations)
Refactored the rest of the heightmap code, which should make it more stable/safe in the long-run
Removed some unnecessary debug logging code
Fixed a bug that caused some settings to corrupt on first install, also likely fixes some settings-related crashes
0.2.10
Fixed custom game speeds not working when "Open RE_Kenshi settings on startup" was disabled
Fixed typo in error logging code
Re-wrote half the installer, fixing multiple bugs + adding better logging
Installer translation fixes
Changed 2nd default custom game speed value from 3 to 2 to better match vanilla
0.2.9
Added German translation (WIP) (Credit to Boron)
Added emergency save system (allows saving on crash)
Crash/bug reporter improvements
Bugfix for RE_Kenshi settings crash
Bugfix for handling of includes in the shader cache
Added more error-checking in the installer
Other misc. installer improvements
Bugfix for loading soundbanks from Steam Workshop mod folders
Bugfix for values at the edge of the map sometimes being incorrect when using the compressed heightmap
Heightmap compression optimization (decodes twice as fast)
Removed support for Steam 1.0.62
0.2.8
Added support for Steam 1.0.62
Added support for Steam 1.0.64
Added bug/crash reporter
Added sliders for max squad/faction size
Removed support for Steam 1.0.60
Removed support for Steam 1.0.59
Russian translation update (Credit to Hack)
0.2.7
Added support for Steam 1.0.60
Bugfix for Boot_Up_Game sound event being triggered before soundbanks are loaded (fixes music at main menu)
Bugfix for printing of wstring audio IDs
Possible bugfix for near RWX allocator race condition (retries 10 times before erroring)
Added hooks/logging for wstring PostEvent
Fix for Kenshi's font size bug (Kenshi didn't apply font sizes correctly on startup) (Steam 1.0.55 + GOG 1.0.59 only - fixed in 1.0.60)
Added Russian translation (Credit to Hack)
0.2.6
Bugfix for crash commonly triggered on first run after boot
Multiple bugfixes for shader cache crashes
Added ability to skip releases to the update checker
Added toggle to disable custom game speed controls (reverts them back to vanilla)
Added setting for disabling RE_Kenshi settings window opening on startup
Added tutorial window for the custom game speed controls
Bugfix for RE_Kenshi sometimes not initializing before/at the main menu (from update checker stalling)
0.2.5
Fixed handling of nullptr in several sound hooks (caused crash in Ashlands)
Fixed resolution scaling of RE_Kenshi settings menu
Bugfix for vanilla soundbanks not loading after main menu (stopped music working)
Fixed race condition in soundbank load/init
Improved handling of settings file corruption
Partial fix for crash on shader compile error in shader cache
0.2.4
Added sound event logging
Added soundbank overriding
Added ability to increase max. camera height
Added shader cache (reduces load time + might reduce stutter while loading)
Bugfix for "RE_Kenshi settings" button not appearing for some languages
Removed 1.0.55 GOG support
Removed 1.0.51 Steam support
Removed 1.0.51 GOG support
0.2.3
Added RNG fix
Added support for 1.0.59
0.2.2:
Added update checker
Added mod API for overriding hard-coded file paths
Added GOG 1.0.55 support
Fixed instant crash when running on unknown game versions
Fixes for handling of missing/default RE_Kenshi settings
Fix for time controls reverting to vanilla on alt-tab/resolution change
0.2.1:
Bugfix for "spikey" map
0.2.0:
Added GUI
Added hooks for game speed keybinds (default f2, f3, f4)
Added heightmap compression
Added attack slots slider