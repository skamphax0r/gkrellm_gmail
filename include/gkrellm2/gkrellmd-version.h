/* GKrellM
 * Copyright (C) 2024 Stefan Gehn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * Additional permission under GNU GPL version 3 section 7:
 *
 * If you modify this program, or any covered work, by linking or
 * combining it with the OpenSSL project's OpenSSL library (or a
 * modified version of that library), containing parts covered by
 * the terms of the OpenSSL or SSLeay licenses, you are granted
 * additional permission to convey the resulting work.
 * Corresponding Source for a non-source form of such a combination
 * shall include the source code for the parts of OpenSSL used as well
 * as that of the covered work.
 */

#ifndef GKRELLMD_VERSION_H
#define GKRELLMD_VERSION_H

#define GKRELLMD_VERSION_MAJOR 2
#define GKRELLMD_VERSION_MINOR 4
#define GKRELLMD_VERSION_REV 0

//! @deprecated
#define GKRELLMD_EXTRAVERSION ""

#define GKRELLMD_CHECK_VERSION(major,minor,rev) \
	(GKRELLMD_VERSION_MAJOR > (major) || \
	(GKRELLMD_VERSION_MAJOR == (major) && GKRELLMD_VERSION_MINOR > (minor)) || \
	(GKRELLMD_VERSION_MAJOR == (major) && GKRELLMD_VERSION_MINOR == (minor) && \
	GKRELLMD_VERSION_REV >= (rev)))

#endif