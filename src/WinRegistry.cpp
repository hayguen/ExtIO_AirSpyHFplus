/*
 *  Windows Registry functions
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

//---------------------------------------------------------------------------

#include "WinRegistry.h"
#include <string.h>

WinRegistry::WinRegistry(KeyT key, const char * subkey, OpenModeT mode)
{
	HKEY hKey;
	LONG ret = 0;
	DWORD disposition = 0;
	switch (key)
	{
	default:
	case HKLM: hKey = HKEY_LOCAL_MACHINE; break;
	case HKCU: hKey = HKEY_CURRENT_USER;  break;
	}
	switch (mode)
	{
	case OPEN:
		ret = RegOpenKeyA(hKey, subkey, &mKey);
		mCreated = false;
		mInitOk = (ret == ERROR_SUCCESS);
		break;
	case CREATE:
		ret = RegCreateKeyExA(hKey, subkey
			, 0		/* Reserved */
			, NULL	/* lpClass */
			, 0		/* dwOptions */
			, NULL	/* samDesired */
			, NULL	/* lpSecurityAttributes */
			, &mKey	/* phkResult */
			, &disposition	/* lpdwDisposition */
			);
		mCreated = ((REG_CREATED_NEW_KEY & disposition) != 0);
		mInitOk = (ret == ERROR_SUCCESS);
		break;
	}
}

WinRegistry::~WinRegistry()
{
	if (mInitOk)
		RegCloseKey(mKey);
}

const char * WinRegistry::get(const char * name, bool * pbOk) const
{
	*pbOk = false;
	if (!mInitOk)
		return 0;

	DWORD type = 0;
	DWORD len = sizeof(macBuf);
	LONG ret = RegQueryValueExA(mKey, name, 0, &type, (LPBYTE)(&macBuf), &len);
	if (ret != ERROR_SUCCESS || REG_SZ != type || len >= sizeof(macBuf) - 1)
		return 0;

	*pbOk = true;
	return macBuf;
}

int WinRegistry::escapeSZ(const char * value, bool * pbOk)
{
	int inpDataLen = strlen(value) + 1;	/* length of data including terminating zero */
	int outDataLen = 0;
	*pbOk = true;
	for (int i = 0; i < inpDataLen; ++i)
	{
		if (value[i] == '\\')
		{
			if (outDataLen < BUFLEN - 2)
			{
				macBuf[outDataLen++] = '\\';	// additional escape for REG_SZ
				macBuf[outDataLen++] = value[i];
			}
			else
			{
				macBuf[outDataLen++] = 0;	// cut string
				*pbOk = false;
				break;
			}
		}
		else
		{
			if (outDataLen < BUFLEN - 1)
				macBuf[outDataLen++] = value[i];
			else
			{
				macBuf[BUFLEN - 1] = 0;
				*pbOk = false;
				break;
			}
		}
	}
	return outDataLen;
}

void WinRegistry::set(const char * name, const char * value, bool * pbOk)
{
	DWORD	type = REG_SZ;
	DWORD	dataLen = escapeSZ(value, pbOk);
	LONG ret = RegSetValueExA(mKey, name
		, 0		/* Reserved */
		, type	/* dwType */
		, (const BYTE *)macBuf	/* lpData */
		, dataLen
		);
	*pbOk = *pbOk && (ret == ERROR_SUCCESS);
}

//---------------------------------------------------------------------------
