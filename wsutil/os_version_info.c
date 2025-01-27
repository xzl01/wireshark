/* os_version_info.c
 * Routines to report operating system version information
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>
#include <errno.h>

#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif

#ifdef HAVE_MACOS_FRAMEWORKS
#include <CoreFoundation/CoreFoundation.h>
#include <wsutil/cfutils.h>
#endif

#include <glib.h>

#include <wsutil/unicode-utils.h>

#include <wsutil/os_version_info.h>

/*
 * Handles the rather elaborate process of getting OS version information
 * from macOS (we want the macOS version, not the Darwin version, the latter
 * being easy to get with uname()).
 */
#ifdef HAVE_MACOS_FRAMEWORKS

/*
 * Fetch a string, as a UTF-8 C string, from a dictionary, given a key.
 */
static char *
get_string_from_dictionary(CFPropertyListRef dict, CFStringRef key)
{
	CFStringRef cfstring;

	cfstring = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)dict,
	    (const void *)key);
	if (cfstring == NULL)
		return NULL;
	if (CFGetTypeID(cfstring) != CFStringGetTypeID()) {
		/* It isn't a string.  Punt. */
		return NULL;
	}
	return CFString_to_C_string(cfstring);
}

/*
 * Get the macOS version information, and append it to the GString.
 * Return TRUE if we succeed, FALSE if we fail.
 *
 * XXX - this gives the OS name as "Mac OS X" even if Apple called/calls
 * it "OS X" or "macOS".
 */
static gboolean
get_macos_version_info(GString *str)
{
	static const UInt8 server_version_plist_path[] =
	    "/System/Library/CoreServices/ServerVersion.plist";
	static const UInt8 system_version_plist_path[] =
	    "/System/Library/CoreServices/SystemVersion.plist";
	CFURLRef version_plist_file_url;
	CFReadStreamRef version_plist_stream;
	CFDictionaryRef version_dict;
	char *string;

	/*
	 * On macOS, report the macOS version number as the OS, and put
	 * the Darwin information in parentheses.
	 *
	 * Alas, Gestalt() is deprecated in Mountain Lion, so the build
	 * fails if you treat deprecation warnings as fatal.  I don't
	 * know of any replacement API, so we fall back on reading
	 * /System/Library/CoreServices/ServerVersion.plist if it
	 * exists, otherwise /System/Library/CoreServices/SystemVersion.plist,
	 * and using ProductUserVisibleVersion.  We also get the build
	 * version from ProductBuildVersion and the product name from
	 * ProductName.
	 */
	version_plist_file_url = CFURLCreateFromFileSystemRepresentation(NULL,
	    server_version_plist_path, sizeof server_version_plist_path - 1,
	    false);
	if (version_plist_file_url == NULL)
		return FALSE;
	version_plist_stream = CFReadStreamCreateWithFile(NULL,
	    version_plist_file_url);
	CFRelease(version_plist_file_url);
	if (version_plist_stream == NULL)
		return FALSE;
	if (!CFReadStreamOpen(version_plist_stream)) {
		CFRelease(version_plist_stream);

		/*
		 * Try SystemVersion.plist.
		 */
		version_plist_file_url = CFURLCreateFromFileSystemRepresentation(NULL,
		    system_version_plist_path, sizeof system_version_plist_path - 1,
		    false);
		if (version_plist_file_url == NULL)
			return FALSE;
		version_plist_stream = CFReadStreamCreateWithFile(NULL,
		    version_plist_file_url);
		CFRelease(version_plist_file_url);
		if (version_plist_stream == NULL)
			return FALSE;
		if (!CFReadStreamOpen(version_plist_stream)) {
			CFRelease(version_plist_stream);
			return FALSE;
		}
	}
#ifdef HAVE_CFPROPERTYLISTCREATEWITHSTREAM
	version_dict = (CFDictionaryRef)CFPropertyListCreateWithStream(NULL,
	    version_plist_stream, 0, kCFPropertyListImmutable,
	    NULL, NULL);
#else
	version_dict = (CFDictionaryRef)CFPropertyListCreateFromStream(NULL,
	    version_plist_stream, 0, kCFPropertyListImmutable,
	    NULL, NULL);
#endif
	if (version_dict == NULL) {
		CFRelease(version_plist_stream);
		return FALSE;
	}
	if (CFGetTypeID(version_dict) != CFDictionaryGetTypeID()) {
		/* This is *supposed* to be a dictionary.  Punt. */
		CFRelease(version_dict);
		CFReadStreamClose(version_plist_stream);
		CFRelease(version_plist_stream);
		return FALSE;
	}
	/* Get the product name string. */
	string = get_string_from_dictionary(version_dict,
	    CFSTR("ProductName"));
	if (string == NULL) {
		CFRelease(version_dict);
		CFReadStreamClose(version_plist_stream);
		CFRelease(version_plist_stream);
		return FALSE;
	}
	g_string_append_printf(str, "%s", string);
	g_free(string);

	/* Get the OS version string. */
	string = get_string_from_dictionary(version_dict,
	    CFSTR("ProductUserVisibleVersion"));
	if (string == NULL) {
		CFRelease(version_dict);
		CFReadStreamClose(version_plist_stream);
		CFRelease(version_plist_stream);
		return FALSE;
	}
	g_string_append_printf(str, " %s", string);
	g_free(string);

	/* Get the build string */
	string = get_string_from_dictionary(version_dict,
	    CFSTR("ProductBuildVersion"));
	if (string == NULL) {
		CFRelease(version_dict);
		CFReadStreamClose(version_plist_stream);
		CFRelease(version_plist_stream);
		return FALSE;
	}
	g_string_append_printf(str, ", build %s", string);
	g_free(string);
	CFRelease(version_dict);
	CFReadStreamClose(version_plist_stream);
	CFRelease(version_plist_stream);
	return TRUE;
}
#endif

#ifdef _WIN32
typedef LONG (WINAPI * RtlGetVersionProc) (OSVERSIONINFOEX *);
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0
#endif
#include <stdlib.h>
#endif // _WIN32

/*
 * Get the OS version, and append it to the GString
 */
void
get_os_version_info(GString *str)
{
#if defined(_WIN32)

	OSVERSIONINFOEX win_version_info = {0};
	RtlGetVersionProc RtlGetVersionP = 0;
	LONG version_status = STATUS_ENTRYPOINT_NOT_FOUND; // Any nonzero value should work.

	/*
	 * We want the major and minor Windows version along with other
	 * information. GetVersionEx provides this, but is deprecated.
	 * We use RtlGetVersion instead, which requires a bit of extra
	 * effort.
	 */

	HMODULE ntdll_module = LoadLibrary(_T("ntdll.dll"));
	if (ntdll_module) {
		RtlGetVersionP = (RtlGetVersionProc) GetProcAddress(ntdll_module, "RtlGetVersion");
	}

	if (RtlGetVersionP) {
		win_version_info.dwOSVersionInfoSize = sizeof(win_version_info);
		version_status = RtlGetVersionP(&win_version_info);
	}

	if (ntdll_module) {
		FreeLibrary(ntdll_module);
	}

	if (version_status != STATUS_SUCCESS) {
		/*
		 * XXX - get the failure reason.
		 */
		g_string_append(str, "unknown Windows version");
		return;
	}

	SYSTEM_INFO system_info;
	memset(&system_info, '\0', sizeof system_info);
	/* Look for and use the GetNativeSystemInfo() function to get the correct processor architecture
	 * even when running 32-bit Wireshark in WOW64 (x86 emulation on 64-bit Windows) */
	GetNativeSystemInfo(&system_info);

	switch (win_version_info.dwPlatformId) {

	case VER_PLATFORM_WIN32s:
		/* Shyeah, right. */
		g_string_append_printf(str, "Windows 3.1 with Win32s");
		break;

	case VER_PLATFORM_WIN32_WINDOWS:
		/*
		 * Windows OT.
		 *
		 *   https://nsis-dev.github.io/NSIS-Forums/html/t-128527.html
		 *
		 * claims that
		 *
		 *   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion
		 *
		 * has a key ProductName, at least in Windows M3, the
		 * value of that key appears to be an OS product name.
		 */
		switch (win_version_info.dwMajorVersion) {

		case 4:
			/* 3 cheers for Microsoft marketing! */
			switch (win_version_info.dwMinorVersion) {

			case 0:
				g_string_append_printf(str, "Windows 95");
				break;

			case 10:
				g_string_append_printf(str, "Windows 98");
				break;

			case 90:
				g_string_append_printf(str, "Windows Me");
				break;

			default:
				g_string_append_printf(str, "Windows OT, unknown version %lu.%lu",
				    win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
				break;
			}
			break;

		default:
			g_string_append_printf(str, "Windows OT, unknown version %lu.%lu",
			    win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
			break;
		}
		break;

	case VER_PLATFORM_WIN32_NT:
		/*
		 * Windows NT.
		 *
		 *   https://stackoverflow.com/a/19778234/16139739
		 *
		 * claims that
		 *
		 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
		 *
		 * has a key ProductName that is "present for Windows XP
		 * and aboeve[sic]".  The value of that key gives a
		 * "product name"...
		 *
		 * ...at least until Windows 11, which it insists is
		 * Windows 10.  So we don't bother with it.  (It may
		 * indicate whether it's Home or Pro or..., but that's
		 * not worth the effort of fixing the "Windows 11 is
		 * Windows 10" nonsense.)
		 *
		 *   https://patents.google.com/patent/EP1517235A2/en
		 *
		 * is a Microsoft patent that mentions the
		 * BrandingFormatString() routine, and seems to suggest
		 * that it dates back to at least Windows XP.
		 *
		 *   https://dennisbabkin.com/blog/?t=how-to-tell-the-real-version-of-windows-your-app-is-running-on
		 *
		 * says that routine is in an undocumented winbrand.dll DLL,
		 * but is used by Microsoft's own code to put the OS
		 * product name into messages.  It, unlike ProductName,
		 * appears to make a distinction between Windows 10 and
		 * Windows 11, and, when handed the string "%WINDOWS_LONG%",
		 * gives the same edition decoration that I suspect
		 * ProductName does.
		 */
		switch (win_version_info.dwMajorVersion) {

		case 3:
		case 4:
			/* NT 3.x and 4.x. */
			g_string_append_printf(str, "Windows NT %lu.%lu",
			    win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
			break;

		case 5:
			/*
			 * W2K, WXP, and their server versions.
			 * 3 cheers for Microsoft marketing!
			 */
			switch (win_version_info.dwMinorVersion) {

			case 0:
				g_string_append_printf(str, "Windows 2000");
				break;

			case 1:
				g_string_append_printf(str, "Windows XP");
				break;

			case 2:
				if ((win_version_info.wProductType == VER_NT_WORKSTATION) &&
				    (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)) {
					g_string_append_printf(str, "Windows XP Professional x64 Edition");
				} else {
					g_string_append_printf(str, "Windows Server 2003");
					if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
						g_string_append_printf(str, " x64 Edition");
				}
				break;

			default:
				g_string_append_printf(str, "Windows NT, unknown version %lu.%lu",
						       win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
				break;
			}
			break;

		case 6: {
			/*
			 * Vista, W7, W8, W8.1, and their server versions.
			 */
			gboolean is_nt_workstation;

			if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
				g_string_append(str, "64-bit ");
			else if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
				g_string_append(str, "32-bit ");
#ifndef VER_NT_WORKSTATION
#define VER_NT_WORKSTATION 0x01
			is_nt_workstation = ((win_version_info.wReserved[1] & 0xff) == VER_NT_WORKSTATION);
#else
			is_nt_workstation = (win_version_info.wProductType == VER_NT_WORKSTATION);
#endif
			switch (win_version_info.dwMinorVersion) {
			case 0:
				g_string_append_printf(str, is_nt_workstation ? "Windows Vista" : "Windows Server 2008");
				break;
			case 1:
				g_string_append_printf(str, is_nt_workstation ? "Windows 7" : "Windows Server 2008 R2");
				break;
			case 2:
				g_string_append_printf(str, is_nt_workstation ? "Windows 8" : "Windows Server 2012");
				break;
			case 3:
				g_string_append_printf(str, is_nt_workstation ? "Windows 8.1" : "Windows Server 2012 R2");
				break;
			default:
				g_string_append_printf(str, "Windows NT, unknown version %lu.%lu",
						       win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
				break;
			}
			break;
		}  /* case 6 */

		case 10: {
			/*
			 * W10, W11, and their server versions.
			 */
			gboolean is_nt_workstation;
                        TCHAR ReleaseId[10];
                        DWORD ridSize = _countof(ReleaseId);

			if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
				g_string_append(str, "64-bit ");
			else if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
				g_string_append(str, "32-bit ");
			is_nt_workstation = (win_version_info.wProductType == VER_NT_WORKSTATION);
			switch (win_version_info.dwMinorVersion) {
			case 0:
				/* List of BuildNumber from https://en.wikipedia.org/wiki/List_of_Microsoft_Windows_versions
				 * and https://docs.microsoft.com/en-us/windows/release-health/windows11-release-information */
				if (is_nt_workstation) {
					if (win_version_info.dwBuildNumber < 10240) {
						/* XXX - W10 builds before 10240? */
						g_string_append_printf(str, "Windows");
					} else if (win_version_info.dwBuildNumber < 22000){
						/* W10 builds sstart at 10240 and end before 22000 */
						g_string_append_printf(str, "Windows 10");
					} else {
						/* Builds 22000 and later are W11 (until there's W12...). */
						g_string_append_printf(str, "Windows 11");
					}
				} else {
					switch (win_version_info.dwBuildNumber) {
					case 14393:
						g_string_append_printf(str, "Windows Server 2016");
						break;
					case 17763:
						g_string_append_printf(str, "Windows Server 2019");
						break;
					case 20348:
						g_string_append_printf(str, "Windows Server 2022");
						break;
					default:
						g_string_append_printf(str, "Windows Server");
						break;
					}
				}

				/*
				 * Windows 10 and 11 have had multiple
				 * releases, with different build numbers.
				 *
				 * The build number *could* be used to
				 * determine the release string, but
				 * that would require a table of releases
				 * and strings, and that would have to
				 * get updated whenever a new release
				 * comes out, and that seems to happen
				 * twice a year these days.
				 *
				 * The good news is that, under
				 *
				 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
				 *
				 * there are two keys, DisplayVersion and
				 * ReleaseId.  If DisplayVersion is present,
				 * it's a string that gives the release
				 * string; if not, ReleaseId gives the
				 * release string.
				 *
				 * The DisplayVersion value is currently
				 * of the form YYHN, where YY is the
				 * last two digits of the year, H stands
				 * for "half", and N is the half of the
				 * year in which it came out.
				 *
				 * The ReleaseId is just a numeric string
				 * and for all the YYHN releases, it's
				 * stuck at the same value.
				 *
				 * Note further that
				 *
				 *   https://github.com/nvaccess/nvda/blob/master/source/winVersion.py
				 *
				 * has a comment claiming that
				 *
				 *   From Version 1511 (build 10586), release
				 *   Id/display version comes from Windows
				 *   Registry.
				 *   However there are builds with no release
				 *   name (Version 1507/10240) or releases
				 *   with different builds.
				 *   Look these up first before asking
				 *   Windows Registry.
				 *
				 * "Look these up first" means "look them
				 * up in a table that goes from
				 *
				 *   10240: Windows 10 1507
				 *
				 * to
				 *
				 *   22621: Windows 11 22H2
				 *
				 * and also includes
				 *
				 *   20348: Windows Server 2022
				 *
				 * I'm not sure why any Windows 10 builds
				 * after 10240 are in the table; what does
				 * "releases with different builds" mean?
				 * does it mean that those particular
				 * builds have bogus ReleaseId or
				 * DisplayVersion values?  Those builds
				 * appear to be official release builds
				 * for W10/W11, according to the table
				 * in
				 *
				 *   https://en.wikipedia.org/wiki/Windows_NT
				 *
				 * so, if those are all necessary, why
				 * should ReleaseId or DisplayVersion be
				 * trusted at all?
				 *
				 * As for the Windows Server 2022 entry,
				 * is that just becuase that script doesn't
				 * bother checking for "workstation" vs.
				 * "server"?
				 */
				if (RegGetValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
				                L"DisplayVersion", RRF_RT_REG_SZ, NULL, &ReleaseId, &ridSize) == ERROR_SUCCESS) {
					g_string_append_printf(str, " (%s)", utf_16to8(ReleaseId));
				}
				else if (RegGetValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
				                L"ReleaseId", RRF_RT_REG_SZ, NULL, &ReleaseId, &ridSize) == ERROR_SUCCESS) {
					g_string_append_printf(str, " (%s)", utf_16to8(ReleaseId));
				}
				break;
			default:
				g_string_append_printf(str, "Windows NT, unknown version %lu.%lu",
						       win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
				break;
			}
			break;
		}  /* case 10 */

		default:
			g_string_append_printf(str, "Windows NT, unknown version %lu.%lu",
			    win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
			break;
		} /* info.dwMajorVersion */
		break;

	default:
		g_string_append_printf(str, "Unknown Windows platform %lu version %lu.%lu",
		    win_version_info.dwPlatformId, win_version_info.dwMajorVersion, win_version_info.dwMinorVersion);
		break;
	}
	if (win_version_info.szCSDVersion[0] != '\0')
		g_string_append_printf(str, " %s", utf_16to8(win_version_info.szCSDVersion));
	g_string_append_printf(str, ", build %lu", win_version_info.dwBuildNumber);
#elif defined(HAVE_SYS_UTSNAME_H)
	struct utsname name;
	/*
	 * We have <sys/utsname.h>, so we assume we have "uname()".
	 */
	if (uname(&name) < 0) {
		g_string_append_printf(str, "unknown OS version (uname failed - %s)",
		    g_strerror(errno));
		return;
	}

	if (strcmp(name.sysname, "AIX") == 0) {
		/*
		 * Yay, IBM!  Thanks for doing something different
		 * from most of the other UNIXes out there, and
		 * making "name.version" apparently be the major
		 * version number and "name.release" be the minor
		 * version number.
		 */
		g_string_append_printf(str, "%s %s.%s", name.sysname, name.version,
		    name.release);
	} else {
		/*
		 * XXX - get "version" on any other platforms?
		 *
		 * On Digital/Tru64 UNIX, it's something unknown.
		 * On Solaris, it's some kind of build information.
		 * On HP-UX, it appears to be some sort of subrevision
		 * thing.
		 * On *BSD and Darwin/macOS, it's a long string giving
		 * a build date, config file name, etc., etc., etc..
		 */
#ifdef HAVE_MACOS_FRAMEWORKS
		/*
		 * On macOS, report the macOS version number as the OS
		 * version if we can, and put the Darwin information
		 * in parentheses.
		 */
		if (get_macos_version_info(str)) {
			/* Success - append the Darwin information. */
			g_string_append_printf(str, " (%s %s)", name.sysname, name.release);
		} else {
			/* Failure - just use the Darwin information. */
			g_string_append_printf(str, "%s %s", name.sysname, name.release);
		}
#else /* HAVE_MACOS_FRAMEWORKS */
		/*
		 * XXX - on Linux, are there any APIs to get the distribution
		 * name and version number?  I think some distributions have
		 * that.
		 *
		 * At least on Linux Standard Base-compliant distributions,
		 * there's an "lsb_release" command.  However:
		 *
		 *	http://forums.fedoraforum.org/showthread.php?t=220885
		 *
		 * seems to suggest that if you don't have the redhat-lsb
		 * package installed, you don't have lsb_release, and that
		 * /etc/fedora-release has the release information on
		 * Fedora.
		 *
		 *	http://linux.die.net/man/1/lsb_release
		 *
		 * suggests that there's an /etc/distrib-release file, but
		 * it doesn't indicate whether "distrib" is literally
		 * "distrib" or is the name for the distribution, and
		 * also speaks of an /etc/debian_version file.
		 *
		 * "lsb_release" apparently parses /etc/lsb-release, which
		 * has shell-style assignments, assigning to, among other
		 * values, DISTRIB_ID (distributor/distribution name),
		 * DISTRIB_RELEASE (release number of the distribution),
		 * DISTRIB_DESCRIPTION (*might* be name followed by version,
		 * but the manpage for lsb_release seems to indicate that's
		 * not guaranteed), and DISTRIB_CODENAME (code name, e.g.
		 * "licentious" for the Ubuntu Licentious Lemur release).
		 * the lsb_release man page also speaks of the distrib-release
		 * file, but Debian doesn't have one, and Ubuntu 7's
		 * lsb_release command doesn't look for one.
		 *
		 * I've seen references to /etc/redhat-release as well.
		 *
		 * At least on my Ubuntu 7 system, /etc/debian_version
		 * doesn't contain anything interesting (just some Debian
		 * codenames).
		 *
		 * See also
		 *
		 *	http://bugs.python.org/issue1322
		 *
		 *	http://www.novell.com/coolsolutions/feature/11251.html
		 *
		 *	http://linuxmafia.com/faq/Admin/release-files.html
		 *
		 * and the Lib/Platform.py file in recent Python 2.x
		 * releases.
		 *
		 * And then there's
		 *
		 *	http://0pointer.de/blog/projects/os-release
		 *
		 * which, apparently, is something that all distributions
		 * with systemd have, which seems to mean "most distributions"
		 * these days.  It also has a list of several of the assorted
		 * *other* such files that various distributions have.
		 *
		 * Maybe look at what pre-version-43 systemd does?  43
		 * removed support for the old files, but I guess that
		 * means older versions *did* support them:
		 *
		 *	https://lists.freedesktop.org/archives/systemd-devel/2012-February/004475.html
		 */
		g_string_append_printf(str, "%s %s", name.sysname, name.release);
#endif /* HAVE_MACOS_FRAMEWORKS */
	}
#else
	g_string_append(str, "an unknown OS");
#endif
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
