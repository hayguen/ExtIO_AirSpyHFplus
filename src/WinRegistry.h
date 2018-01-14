/*
 * Windows Registry functions
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WIN_REGISTRY_H
#define WIN_REGISTRY_H

#include <Windows.h>

class WinRegistry
{
public:
	typedef enum { HKLM, HKCU } KeyT;
	typedef enum { OPEN, CREATE } OpenModeT;

	WinRegistry(KeyT key, const char * subkey, OpenModeT mode);
	~WinRegistry();

	bool ok() const { return mInitOk; }
	bool created() const { return mCreated; }
	const char * get(const char * name, bool * pbOk) const;
	void set(const char * name, const char * value, bool * pbOk);

private:
	int escapeSZ(const char * value, bool * pbOk);

	HKEY mKey;
	bool mInitOk;
	bool mCreated;
	static const int BUFLEN = 4096;
	mutable char macBuf[BUFLEN];
};

#endif
