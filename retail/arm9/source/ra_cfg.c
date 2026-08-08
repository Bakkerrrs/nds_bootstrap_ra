/*
    Step 3c's half that needs no network: the RetroAchievements configuration file.

    Deliberately shaped like the one odelot ships for his MiSTer core -- `key=value`, `#` for
    comments, credentials at the top -- because that is the file this project's user already
    knows, and a format someone can copy from a working example is worth more than a tidier one
    they have to learn.

    ## The password lives in the file

    That is a decision on the record rather than an oversight: it is how odelot's works, and it
    was chosen knowingly after the alternative -- log in once, keep only the token, never write
    the password to the card -- was offered and declined. `r=login` therefore sends it in the
    clear on every boot, and the file sits readable on the SD.

    What this code does *not* do is put it anywhere else. It never goes in the log, which is a
    file that gets shared: see raConfigRedact(). Nothing else about the choice is this file's
    business.

    ## Everything unknown is tolerated, and everything recognised-but-unused says so

    odelot's file carries a dozen keys about popups, leaderboards and hardcore behaviour. Most
    of them describe an overlay this fork does not have yet. Rejecting them would make his file
    unusable here for no reason; accepting them silently would make a typo indistinguishable
    from a feature that is simply not built. So the ones we know about are counted as
    recognised-but-not-yet, and only genuinely unknown keys are reported.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <stdio.h>
#include <string.h>

/*
    Keys this fork acts on today. Anything here changes behaviour.
*/
#define RA_CFG_USERNAME  "username"
#define RA_CFG_PASSWORD  "password"
#define RA_CFG_HARDCORE  "hardcore"
#define RA_CFG_DEBUG     "debug"

/*
    Keys odelot's file has that this fork parses and then does nothing with, because what they
    control does not exist here yet -- the overlay has no font, so there are no popups to show
    or hide, and there are no leaderboards. Listed so his file loads without complaint and a
    misspelling still gets one.
*/
static const char* const raCfgNotYet[] = {
	"show_challenge_show_popup",
	"show_challenge_hide_popup",
	"show_progress_popups",
	"show_progress_name",
	"show_leaderboards_updates",
	"show_leaderboards_submission",
	"force_hardcore",
	"multiline_desc",
	"list_desc_ticker",
	"list_hotkey",
	NULL,
};

static char* raCfgTrim(char* s) {
	char* end;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = 0;
	return s;
}

static void raCfgCopy(char* dst, size_t size, const char* src) {
	size_t n = strlen(src);

	if (n > size - 1) {
		n = size - 1;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

static bool raCfgFlag(const char* value) {
	/* `1` is yes in odelot's file. Anything else, including empty, is no. */
	return value[0] == '1' && value[1] == 0;
}

static bool raCfgKnownUnused(const char* key) {
	int i;

	for (i = 0; raCfgNotYet[i]; i++) {
		if (strcmp(key, raCfgNotYet[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool raConfigRead(const char* path, raConfig* cfg) {
	char  line[192];
	FILE* file;

	memset(cfg, 0, sizeof(*cfg));

	file = fopen(path, "r");
	if (!file) {
		return false;
	}
	cfg->found = 1;

	while (fgets(line, sizeof(line), file)) {
		char* key;
		char* value;
		char* eq;

		key = raCfgTrim(line);
		if (key[0] == 0 || key[0] == '#') {
			continue;
		}

		eq = strchr(key, '=');
		if (!eq) {
			cfg->badLines++;
			continue;
		}
		*eq   = 0;
		value = raCfgTrim(eq + 1);
		key   = raCfgTrim(key);

		if (strcmp(key, RA_CFG_USERNAME) == 0) {
			raCfgCopy(cfg->username, sizeof(cfg->username), value);
		} else if (strcmp(key, RA_CFG_PASSWORD) == 0) {
			raCfgCopy(cfg->password, sizeof(cfg->password), value);
		} else if (strcmp(key, RA_CFG_HARDCORE) == 0) {
			cfg->hardcore = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_DEBUG) == 0) {
			cfg->debug = raCfgFlag(value);
		} else if (raCfgKnownUnused(key)) {
			cfg->notYet++;
		} else {
			cfg->unknown++;
		}
	}
	fclose(file);

	cfg->usable = (cfg->username[0] != 0 && cfg->password[0] != 0);
	return true;
}

/*
    What may be said about the file out loud.

    The log is written to the SD card and then sent to whoever is reading the round -- that is
    the entire point of it -- so the password must not be in it, and neither must the token,
    which grants exactly the same power over the account. A length is enough to tell "the field
    is empty" from "the field is set", which is the only question a log needs to answer.
*/
const char* raConfigRedact(const char* secret) {
	static char shown[24];

	if (!secret || !secret[0]) {
		return "(empty)";
	}
	sniprintf(shown, sizeof(shown), "(set, %u chars)", (unsigned)strlen(secret));
	return shown;
}

#endif /* RA_LAUNCHER_WIFI */
