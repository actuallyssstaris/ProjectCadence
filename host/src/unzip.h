#ifndef OSU5_UNZIP_H
#define OSU5_UNZIP_H

#include <stdbool.h>

/* Extracts a zip (.osk / .osz) into dir, dropping a single top-level folder that
 * wraps everything. lower: lowercase the names, for skins, which are made on
 * Windows and looked up by fixed lowercase names here. Maps keep theirs, the .osu
 * refers to its files by name. Returns files written, -1 on a broken archive. */
int unzip_to(const char *zip_path, const char *dir, bool lower);

#endif
