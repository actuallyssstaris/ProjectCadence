#ifndef OSU5_UNZIP_H
#define OSU5_UNZIP_H

/* Extracts a zip (.osk / .osz) into dir. Names are lowercased, since skins are
 * made on Windows and the console's filesystem cares about case, and a single
 * top-level folder wrapping everything is dropped. Returns files written, -1 on a
 * broken archive. */
int unzip_to(const char *zip_path, const char *dir);

#endif
