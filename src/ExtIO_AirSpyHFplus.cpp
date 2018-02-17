/*
 * ExtIO wrapper
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

#define LIBEXTIO_EXPORTS 1


#include <stdint.h>

#include <Windows.h>
#include <WindowsX.h>
#include <commctrl.h>
#include <process.h>
#include <tchar.h>

#include <new>
#include <algorithm>
#include <stdio.h>

#include "resource.h"
#include "ExtIO_AirSpyHFplus.h"
#include "LC_ExtIO_Types.h"
#include "WinRegistry.h"

#include <airspyhf.h>


#ifdef _MSC_VER
	#pragma warning(disable : 4996)
	#define snprintf  _snprintf
#endif

#define EXTIO_BLOCKLEN_IN_IQ_FRAMES		8192

// allow up to +/- 200 ppm = +/- 200 000 ppb
#define MAX_PPB		( 200 * 1000)
#define MIN_PPB		(-200 * 1000)

static char airspyhf_lib_gitver[128] = "\0";


#define MAX_NUM_DEVS	8

static uint64_t dev_serials[MAX_NUM_DEVS];
static int num_dev_serials = 0;

static uint64_t combo_serials[MAX_NUM_DEVS];
static int num_combo_serials = 0;

static airspyhf_device_t* dev = 0;
static int devIdx = 0;
static int retOpen = AIRSPYHF_ERROR;
static int retLast = AIRSPYHF_ERROR;
static bool devStreaming = false;

static uint64_t sel_DeviceSerial = 0;	// last selected one
static uint64_t cfg_DeviceSerial = 0;	// initially configured one

static bool changedSelectionSinceFailedOpen = false;

static uint32_t	numSrates = 1;
static uint32_t	dev_srates[EXTIO_MAX_SRATE_VALUES] =
{ 768000, 0, 0, 0, 0, 0, 0, 0
, 0, 0, 0, 0, 0, 0, 0, 0
, 0, 0, 0, 0, 0, 0, 0, 0
, 0, 0, 0, 0, 0, 0, 0, 0
};

static int num_combo_srates = 0;
static uint32_t	combo_srates[EXTIO_MAX_SRATE_VALUES];


static int n_srates = 0;
static extHWtypeT extHWtype = exthwUSBfloat32;  /* default ExtIO type 16-bit samples */

static uint64_t dropped_sample_count = 0;

// forward
int airspyhf_sample_block_cb(airspyhf_transfer_t* transfer_fn);



static bool SDRsupportsLogging = false;
static bool SDRcalledSettings = false;

static volatile long LO_Frequency = 10 * 1000 * 1000L;	// default: 10 MHz

static volatile int SampleRateIdx = 0;		// default = 2.3 MSps

static volatile int32_t FreqCorrPPB = 0;

// GPIO Pins 0 - 3
static volatile int gpioA = 0;
static volatile int gpioB = 0;
static volatile int gpioC = 0;
static volatile int gpioD = 0;


static airspyhf_complex_float_t extio_sample_buffer[EXTIO_BLOCKLEN_IN_IQ_FRAMES * 2];	// 2 for I and Q. 2 times
static int extio_sample_buffer_size = 0;


// Thread handle
static bool GUIDebugConnection = false;
static volatile HANDLE worker_handle=INVALID_HANDLE_VALUE;


/* ExtIO Callback */
// void(*ExtIOCallBack)(int, int, float, void *) = NULL;
pfnExtIOCallback ExtIOCallBack = NULL;


static char  dev_versionStr[128];
static TCHAR dev_versionStrT[128] = TEXT("-");

static char  statusStr[256];
static TCHAR statusStrT[256] = TEXT("-");

static uint64_t SNtext2Serial(const char * text, bool setSelected);
static const char * Serial2SNtextC(uint64_t deviceSerial);
static const TCHAR * Serial2SNtextT(uint64_t deviceSerial);

static void updateDeviceListCB(HWND hwndDlg, bool bReFillCB = true);
static void updateSampleRatesCB(HWND hwndDlg, bool callback, bool bReFillCB = true);
static void updateGPIOCBs(HWND hwndDlg);
static void updateDevVerStrCB(HWND hwndDlg);
static void updateLibVerStrCB(HWND hwndDlg);
static void updateFreqCorrCB(HWND hwndDlg);
static void updateStatusCB(HWND hwndDlg);
static void setStatusCB(const char * text, bool bError = false);

extern "C" void  LIBEXTIO_API __stdcall ExtIoSetSetting(int idx, const char * value);
extern "C" int   LIBEXTIO_API __stdcall ExtIoGetSetting(int idx, char * description, char * value);

static INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
static HWND h_dialog=NULL;
static bool finishedInitDialog = false;


// error message, with "const char*" in IQdata,
//   intended for a log file  AND  a message box
// for messages extHw_MSG_*

static char acLogMsg[1024];

#define SDRLOGTXT( A, TEXT )	\
	do {												\
		if ( ExtIOCallBack && SDRsupportsLogging )		\
			ExtIOCallBack(-1, A, 0, TEXT );			\
		if (extHw_MSG_ERRDLG == A)						\
			setStatusCB(TEXT, true);					\
	} while (0)

#define SDRLOG( A, TEXTFMT, ... )	do { snprintf(acLogMsg, 1023, TEXTFMT, ##__VA_ARGS__); SDRLOGTXT(A, acLogMsg); } while (0)


extern "C"
bool  LIBEXTIO_API __stdcall InitHW(char *name, char *model, int& type)
{
	if (!SDRcalledSettings)
	{
		WinRegistry reg(WinRegistry::HKCU, "Software\\ExtIO_AirSpyHFplus", WinRegistry::OPEN);
		if (reg.ok())
		{
			char name[32];
			const char * value = nullptr;
			bool bOk;
			for (int idx = 0; idx < 1000; ++idx)
			{
				snprintf(name, 31, "%03d_key", idx);
				value = reg.get(name, &bOk);
				if (!bOk)
					break;
				ExtIoSetSetting(idx, value);
			}
		}
	}

	strcpy_s(name,63, "AirSpy HF+");
	strcpy_s(model,15,"HF+");
	name[63]=0;
	model[15]=0;

	extHWtype = exthwUSBfloat32;
	type = extHWtype;

	return TRUE;
}

extern "C"
int LIBEXTIO_API __stdcall GetStatus()
{
	/* dummy function */
    return 0;
}


static void setupDevice()
{
	// initialize GPIOs
	airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_0, (gpioA ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
	airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_1, (gpioB ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
	airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_2, (gpioC ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
	airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_3, (gpioD ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));

	// set frequency correction
	int32_t prev = FreqCorrPPB;
	int32_t value = FreqCorrPPB;
	if (AIRSPYHF_SUCCESS == airspyhf_get_calibration(dev, &value))
	{
		FreqCorrPPB = value;
		if (h_dialog)
			updateFreqCorrCB(h_dialog);
	}
	else
	{
		FreqCorrPPB = prev;
		airspyhf_set_calibration(dev, FreqCorrPPB);
	}

	// clear i/q buffer
	extio_sample_buffer_size = 0;
}


extern "C"
bool  LIBEXTIO_API __stdcall OpenHW()
{
	SDRLOGTXT(extHw_MSG_DEBUG, "OpenHW()");
	airspyhf_lib_version_t lib_version;
	airspyhf_lib_version(&lib_version);
	snprintf(airspyhf_lib_gitver, 127, "%d.%d.%d"
		, lib_version.major_version, lib_version.minor_version, lib_version.revision);
	SDRLOG(extHw_MSG_DEBUG, "OpenHW(): libairspyhf version '%s'", airspyhf_lib_gitver);
	if (!h_dialog)
	{
		h_dialog = CreateDialog(hInst, MAKEINTRESOURCE(IDD_EXTIO_SETTINGS), NULL, (DLGPROC)MainDlgProc);
		ShowWindow(h_dialog, SW_HIDE);
	}

	setStatusCB("");

	num_dev_serials = airspyhf_list_devices(&dev_serials[0], MAX_NUM_DEVS);
	{
		SDRLOG(extHw_MSG_DEBUG, "OpenHW(): found %d devices:", num_dev_serials);
		for (int k = 0; k < num_dev_serials; ++k)
			SDRLOG(extHw_MSG_DEBUG, "  device %d: serial %s", k, Serial2SNtextC(dev_serials[k]));
	}

	updateDeviceListCB(h_dialog);
	updateLibVerStrCB(h_dialog);
	updateSampleRatesCB(h_dialog, false, false);
	updateFreqCorrCB(h_dialog);

	snprintf(dev_versionStr, 127, "-");
	changedSelectionSinceFailedOpen = false;

	retLast = retOpen = airspyhf_open_sn(&dev, sel_DeviceSerial);
	if (AIRSPYHF_SUCCESS == retOpen && dev)
	{
		retLast = airspyhf_version_string_read(dev, dev_versionStr, 128);
		if (AIRSPYHF_SUCCESS == retLast)
		{
			SDRLOG(extHw_MSG_DEBUG, "OpenHW(): device version '%s'", dev_versionStr);
		}
		else
			snprintf(dev_versionStr, 127, "<Error>");

		setupDevice();

		retLast = airspyhf_get_samplerates(dev, &numSrates, 0);
		if (AIRSPYHF_SUCCESS == retLast && numSrates > 0)
		{
			SDRLOG(extHw_MSG_DEBUG, "OpenHW(): device with serial %s supports N = %u samplerates:", Serial2SNtextC(sel_DeviceSerial), numSrates);

			if (numSrates > EXTIO_MAX_SRATE_VALUES)
				numSrates = EXTIO_MAX_SRATE_VALUES;
			retLast = airspyhf_get_samplerates(dev, &dev_srates[0], numSrates);
			if (AIRSPYHF_SUCCESS == retLast)
			{
				if (numSrates > 1)
					std::sort(&dev_srates[0], &dev_srates[numSrates]);
				n_srates = numSrates;
				if (SampleRateIdx >= n_srates)
				{
					SampleRateIdx = 0;
					if (ExtIOCallBack)
						EXTIO_STATUS_CHANGE(ExtIOCallBack, extHw_Changed_SampleRate);
				}

				for (uint32_t k = 0; k < numSrates; ++k)
					SDRLOG(extHw_MSG_DEBUG, "  samplerate %u: %u Hz", k, dev_srates[k]);

				updateSampleRatesCB(h_dialog, true);
			}
		}
		else
			retLast = AIRSPYHF_ERROR;
	}
	else
	{
		if (dev)
		{
			airspyhf_close(dev);
			dev = NULL;
			snprintf(dev_versionStr, 127, "-");
		}
	}

	if (!dev)
	{
		if (sel_DeviceSerial)
			SDRLOGTXT(extHw_MSG_ERRDLG, "ERROR: Failed to open selected device!");
		else
			SDRLOGTXT(extHw_MSG_ERRDLG, "ERROR: Failed to open any AirSpy HF+ device!");
	}
	else
		setStatusCB("");

	if (h_dialog)
		PostMessage(h_dialog, WM_COMMAND, (WPARAM)IDC_REFRESH_SRATES, (LPARAM)0);

	// always TRUE! - to allow re-configuration of Serial Number
	return TRUE;
	//return (AIRSPYHF_SUCCESS == retOpen && AIRSPYHF_SUCCESS == retLast) ? TRUE : FALSE;
}


extern "C"
long LIBEXTIO_API __stdcall SetHWLO(long freq)
{
	if (dev)
	{
		retLast = airspyhf_set_freq(dev, uint32_t(freq));
		LO_Frequency = freq;
	}
	return 0;
}


extern "C"
int LIBEXTIO_API __stdcall StartHW(long freq)
{
	const int numIQpairs = EXTIO_BLOCKLEN_IN_IQ_FRAMES;
	SDRLOGTXT(extHw_MSG_DEBUG, "StartHW()");
	if (dev)
		setStatusCB("");
	if (!dev && changedSelectionSinceFailedOpen)
		OpenHW();

	if (dev)
	{
		setupDevice();

		SetHWLO(freq);
		int startResult = airspyhf_start(dev, airspyhf_sample_block_cb, NULL);
		if (AIRSPYHF_SUCCESS == startResult)
		{
			devStreaming = true;
			setStatusCB("Running ..");
		}
		else
		{
			if (dev)
			{
				airspyhf_close(dev);
				dev = NULL;
				snprintf(dev_versionStr, 127, "-");
			}
			if (ExtIOCallBack)
				EXTIO_STATUS_CHANGE(ExtIOCallBack, extHw_Stop);
			//numIQpairs = -1;	// abort StartHW()!
			changedSelectionSinceFailedOpen = true;
			SDRLOGTXT(extHw_MSG_ERRDLG, "ERROR: Failed to start streaming!");
		}
		if (h_dialog)
			PostMessage(h_dialog, WM_COMMAND, (WPARAM)IDC_REFRESH_SRATES, (LPARAM)0);
	}
	else
	{
		if (ExtIOCallBack)
			EXTIO_STATUS_CHANGE(ExtIOCallBack, extHw_Stop);
		//numIQpairs = -1;	// abort StartHW()!
		changedSelectionSinceFailedOpen = true;
		if (sel_DeviceSerial)
			SDRLOGTXT(extHw_MSG_ERRDLG, "ERROR: Failed to open selected device!");
		else
			SDRLOGTXT(extHw_MSG_ERRDLG, "ERROR: Failed to open any AirSpy HF+ device!");
	}

	if ( numIQpairs > 0 )
		SDRLOG(extHw_MSG_DEBUG, "StartHW() = %d. Callback will deliver %d I/Q pairs per call", numIQpairs, numIQpairs);
	return numIQpairs;
}

extern "C"
long LIBEXTIO_API __stdcall GetHWLO()
{
	return LO_Frequency;
}


extern "C"
long LIBEXTIO_API __stdcall GetHWSR()
{
	long sr = 0L;
	if (!n_srates)
		sr = 768000L;
	else if (0 <= SampleRateIdx && SampleRateIdx < n_srates)
		sr = long(dev_srates[SampleRateIdx]);
	return sr;
}

#if 1

// no need if there's only one single samplerate supported

extern "C"
int LIBEXTIO_API __stdcall ExtIoGetSrates( int srate_idx, double * samplerate )
{
	if (0 <= srate_idx && srate_idx < n_srates)
	{
		*samplerate = double(dev_srates[srate_idx]);
		return 0;
	}
	return 1;	// ERROR
}

extern "C"
int  LIBEXTIO_API __stdcall ExtIoGetActualSrateIdx(void)
{
	return SampleRateIdx;
}

extern "C"
int  LIBEXTIO_API __stdcall ExtIoSetSrate( int srate_idx )
{
	if (srate_idx >= 0 && srate_idx < n_srates)
	{
		SampleRateIdx = srate_idx;
		if (h_dialog)
			updateSampleRatesCB(h_dialog, false, false);
		return 0;
	}
	return 1;	// ERROR
}

#endif

static uint64_t SNtext2Serial(const char * text, bool setSelected)
{
	uint64_t deviceSerial = 0;
	if (!strcmp(text, "Auto"))
		deviceSerial = 0;
	else
	{
		char *end = NULL;
		deviceSerial = strtoull(text, &end, 16);
		if (end - text != 16)
			deviceSerial = 0;
	}
	if (setSelected)
		sel_DeviceSerial = deviceSerial;
	return deviceSerial;
}

static const char * Serial2SNtextC(uint64_t deviceSerial)
{
	static char text[32];
	const uint32_t deviceSerialLo = deviceSerial & 0xFFFFFFFF;
	const uint32_t deviceSerialHi = (deviceSerial >> 32) & 0xFFFFFFFF;
	if (deviceSerial)
		snprintf(text, 31, "%08X%08X", deviceSerialHi, deviceSerialLo);
	else
		snprintf(text, 31, "%s", "Auto");
	return text;
}

static const TCHAR * Serial2SNtextT(uint64_t deviceSerial)
{
	static TCHAR text[32];
	const uint32_t deviceSerialLo = deviceSerial & 0xFFFFFFFF;
	const uint32_t deviceSerialHi = (deviceSerial >> 32) & 0xFFFFFFFF;
	if (deviceSerial)
		_stprintf_s(text, 31, TEXT("%08X%08X"), deviceSerialHi, deviceSerialLo);
	else
		_stprintf_s(text, 31, TEXT("Auto"));
	return text;
}

typedef enum
{
	CFG_SELECTED_DEVICE_SN = 0, CFG_AVAILABLE_DEVICE_SNS, CFG_SAMPLERATE, CFG_FREQ_CORR_PPB, CFG_GPIO_A, CFG_GPIO_B, CFG_GPIO_C, CFG_GPIO_D
} ConfigIdx;


extern "C"
int   LIBEXTIO_API __stdcall ExtIoGetSetting(int idx, char * description, char * value)
{
	int k;
	char * dest = value;

	switch (ConfigIdx(idx))
	{
	case CFG_SELECTED_DEVICE_SN:
		snprintf(description, 1024, "%s", "Selected_Device_Serial");
		snprintf(value, 1024, "%s", Serial2SNtextC(sel_DeviceSerial));
		return 0;
	case CFG_AVAILABLE_DEVICE_SNS:
		snprintf(description, 1024, "%s", "Available_Devices");
		for (k = 0; k < num_dev_serials; ++k)
		{
			snprintf(dest, 1023 - (dest - value), "%s, ", Serial2SNtextC(dev_serials[k]));
			dest += strlen(dest);
		}
		return 0;
	case CFG_SAMPLERATE:
		snprintf( description, 1024, "%s", "SampleRateIdx" );
		snprintf(value, 1024, "%d", SampleRateIdx);
		return 0;
	case CFG_FREQ_CORR_PPB:
		snprintf( description, 1024, "%s", "Frequency_Correction_in_PPB" );
		snprintf(value, 1024, "%d", FreqCorrPPB);
		return 0;
	case CFG_GPIO_A:
		snprintf(description, 1024, "%s", "GPIO_0");
		snprintf(value, 1024, "%d", gpioA);
		return 0;
	case CFG_GPIO_B:
		snprintf(description, 1024, "%s", "GPIO_1");
		snprintf(value, 1024, "%d", gpioB);
		return 0;
	case CFG_GPIO_C:
		snprintf(description, 1024, "%s", "GPIO_2");
		snprintf(value, 1024, "%d", gpioC);
		return 0;
	case CFG_GPIO_D:
		snprintf(description, 1024, "%s", "GPIO_3");
		snprintf(value, 1024, "%d", gpioD);
		return 0;
	default:
		return -1;	// ERROR
	}
	return -1;	// ERROR
}

extern "C"
void  LIBEXTIO_API __stdcall ExtIoSetSetting(int idx, const char * value)
{
	SDRcalledSettings = true;
	switch (ConfigIdx(idx))
	{
	case CFG_SELECTED_DEVICE_SN:
		SNtext2Serial(value, true);
		cfg_DeviceSerial = sel_DeviceSerial;
		return;
	case CFG_AVAILABLE_DEVICE_SNS:
		return;
	case CFG_SAMPLERATE:
		SampleRateIdx = atoi(value);
		return;
	case CFG_FREQ_CORR_PPB:
		// don't use this value from settings
		FreqCorrPPB = 0;
		return;
	case CFG_GPIO_A:	gpioA = atoi(value) ? 1 : 0;	return;
	case CFG_GPIO_B:	gpioB = atoi(value) ? 1 : 0;	return;
	case CFG_GPIO_C:	gpioC = atoi(value) ? 1 : 0;	return;
	case CFG_GPIO_D:	gpioD = atoi(value) ? 1 : 0;	return;
	default:	;
	}
	return;
}


extern "C"
void LIBEXTIO_API __stdcall StopHW()
{
	SDRLOG(extHw_MSG_DEBUG, "StopHW()");
	if (dev)
	{
		airspyhf_stop(dev);
		setStatusCB("Stopped.");
	}
	devStreaming = false;
	if (h_dialog)
		PostMessage(h_dialog, WM_COMMAND, (WPARAM)IDC_REFRESH_SRATES, (LPARAM)0);
}

extern "C"
void LIBEXTIO_API __stdcall CloseHW()
{
	SDRLOG(extHw_MSG_DEBUG, "CloseHW()");

	if (devStreaming)
		StopHW();

	if (dev)
	{
		airspyhf_close(dev);
		setStatusCB("Device closed.");
	}
	dev = NULL;
	snprintf(dev_versionStr, 127, "-");

	if (h_dialog)
		DestroyWindow(h_dialog);
	h_dialog = NULL;

	if (!SDRcalledSettings)
	{
		WinRegistry reg(WinRegistry::HKCU, "Software\\ExtIO_AirSpyHFplus", WinRegistry::CREATE);
		if (reg.ok())
		{
			char description[1024 + 4];
			char value[1024 + 4];
			char name[32];
			bool bOk;
			for (int idx = 0; idx < 1000; ++idx)
			{
				int ret = ExtIoGetSetting(idx, description, value);
				if (ret != 0)
					break;
				snprintf(name, 31, "%03d_description", idx);
				reg.set(name, description, &bOk);
				snprintf(name, 31, "%03d_key", idx);
				reg.set(name, value, &bOk);
			}
		}
	}
}

extern "C"
void LIBEXTIO_API __stdcall ShowGUI()
{
	if (h_dialog)
	{
		ShowWindow(h_dialog, SW_SHOW);
		SetForegroundWindow(h_dialog);
		PostMessage(h_dialog, WM_COMMAND, (WPARAM)IDC_REFRESH_SRATES, (LPARAM)0);
	}
}

extern "C"
void LIBEXTIO_API  __stdcall HideGUI()
{
	if (h_dialog)
		ShowWindow(h_dialog,SW_HIDE);
}

extern "C"
void LIBEXTIO_API  __stdcall SwitchGUI()
{
	if (h_dialog)
	{
		if (IsWindowVisible(h_dialog))
			ShowWindow(h_dialog, SW_HIDE);
		else
		{
			ShowWindow(h_dialog, SW_SHOW);
			PostMessage(h_dialog, WM_COMMAND, (WPARAM)IDC_REFRESH_SRATES, (LPARAM)0);
		}
	}
}


extern "C"
void LIBEXTIO_API __stdcall SetCallback(pfnExtIOCallback funcptr)
{
	ExtIOCallBack = funcptr;
}


extern "C"
void LIBEXTIO_API  __stdcall ExtIoSDRInfo(int extSDRInfo, int additionalValue, void * additionalPtr)
{
	if (extSDRInfo == extSDR_supports_Logging)
		SDRsupportsLogging = true;
}


int airspyhf_sample_block_cb(airspyhf_transfer_t* transfer_fn)
{
	// return 0 : all OK, continue transfer
	// else : stop!

	if (!transfer_fn || transfer_fn->sample_count <= 0)
		return 0;

	if (!dropped_sample_count && transfer_fn->dropped_samples)
		SDRLOG(extHw_MSG_ERROR, "airspyhf_sample_block_cb(): DROPPED %u SAMPLES!!! Not logging further drops!", unsigned(transfer_fn->dropped_samples));

	//if (last_sample_count != transfer_fn->sample_count)
	//	SDRLOG(extHw_MSG_DEBUG, "airspyhf_sample_block_cb(): block size %d I/Q pairs.", transfer_fn->sample_count);

	dropped_sample_count += transfer_fn->dropped_samples;

	if (ExtIOCallBack)
	{
		const airspyhf_complex_float_t * pSrc = transfer_fn->samples;	// source are new I/Q frames
		if (extio_sample_buffer_size)
		{
			memcpy(&extio_sample_buffer[extio_sample_buffer_size], pSrc, transfer_fn->sample_count * sizeof(airspyhf_complex_float_t));
			extio_sample_buffer_size += transfer_fn->sample_count;
			pSrc = &extio_sample_buffer[0];		// buffer is new source
		}
		else
			extio_sample_buffer_size = transfer_fn->sample_count;

		// transmit as much as possible from pSrc/extio_sample_buffer_size
		int iCbOffset = 0;
		while (iCbOffset + EXTIO_BLOCKLEN_IN_IQ_FRAMES <= extio_sample_buffer_size)
		{
			ExtIOCallBack(EXTIO_BLOCKLEN_IN_IQ_FRAMES, 0, 0, const_cast<airspyhf_complex_float_t *>(&pSrc[iCbOffset]));
			iCbOffset += EXTIO_BLOCKLEN_IN_IQ_FRAMES;
		}
		// move remaining data to buffer
		extio_sample_buffer_size -= iCbOffset;		// this is remaining amount of I/Q frames
		if (extio_sample_buffer_size)
			memmove(&extio_sample_buffer[0], &pSrc[iCbOffset], extio_sample_buffer_size * sizeof(airspyhf_complex_float_t));
	}
	else
	{
		// no callback? => clear buffer
		extio_sample_buffer_size = 0;
	}

	return 0;
}


static void updateDeviceListCB(HWND hwndDlg, bool bReFillCB)
{
	HWND hDlgItmDevList = GetDlgItem(hwndDlg, IDC_DEV_SERIAL);
	int sel_DevIdx = -1, cfg_DevIdx = -1;	// cfg_DeviceSerial = sel_DeviceSerial;
	if (bReFillCB)
	{
		TCHAR str[256];
		ComboBox_ResetContent(hDlgItmDevList);

		num_combo_serials = 0;
		combo_serials[num_combo_serials++] = 0;	// always add "Auto"
		if (sel_DeviceSerial == combo_serials[num_combo_serials - 1])
			sel_DevIdx = num_combo_serials - 1;
		if (cfg_DeviceSerial == combo_serials[num_combo_serials - 1])
			cfg_DevIdx = num_combo_serials - 1;
		for (int k = 0; k < num_dev_serials; ++k)
		{
			combo_serials[num_combo_serials++] = dev_serials[k];
			if (sel_DeviceSerial == combo_serials[num_combo_serials - 1])
				sel_DevIdx = num_combo_serials - 1;
			if (cfg_DeviceSerial == combo_serials[num_combo_serials - 1])
				cfg_DevIdx = num_combo_serials - 1;
		}
		if (cfg_DevIdx < 0 && cfg_DeviceSerial)	// add "last config"
			combo_serials[num_combo_serials++] = cfg_DeviceSerial;
		if (sel_DevIdx < 0 && cfg_DeviceSerial != sel_DeviceSerial)	// add "last selection"
			combo_serials[num_combo_serials++] = sel_DeviceSerial;

		for (int k = 0; k < num_combo_serials; ++k)
		{
			_stprintf_s(str, 255, TEXT("%s"), Serial2SNtextT(combo_serials[k]));
			ComboBox_AddString(hDlgItmDevList, str);
		}
	}

	const BOOL bEnableControl = (devStreaming ? FALSE : TRUE);
	ComboBox_Enable(hDlgItmDevList, bEnableControl);

	sel_DevIdx = -1;
	for (int k = 0; k < num_combo_serials; ++k)
	{
		if (sel_DeviceSerial == combo_serials[k])
			sel_DevIdx = k;
	}
	ComboBox_SetCurSel(hDlgItmDevList, (sel_DevIdx >= 0 ? sel_DevIdx : 0) );

	SDRLOG(extHw_MSG_DEBUG, "updateDeviceListCB: %s at idx %d", (devStreaming ? "disabling" : "enabling"), sel_DevIdx);
}

static void updateSampleRatesCB(HWND hwndDlg, bool callback, bool bReFillCB)
{
	HWND hDlgItmTunerSR = GetDlgItem(hwndDlg, IDC_SAMPLERATE);
	if (bReFillCB)
	{
		TCHAR str[256];
		ComboBox_ResetContent(hDlgItmTunerSR);
		num_combo_srates = 0;
		for (int k = 0; k < n_srates; ++k)
		{
			combo_srates[num_combo_srates++] = dev_srates[k];
			_stprintf_s(str, 255, TEXT("%u Hz"), dev_srates[k]);
			ComboBox_AddString(hDlgItmTunerSR, str);
		}
		if (!num_combo_srates)
		{
			combo_srates[num_combo_srates++] = 768000;		// have at least one samplerate
			_stprintf_s(str, 255, TEXT("%u Hz"), combo_srates[0]);
			ComboBox_AddString(hDlgItmTunerSR, str);
		}
	}
	if (n_srates && SampleRateIdx >= n_srates)
		SampleRateIdx = 0;

	ComboBox_SetCurSel(hDlgItmTunerSR, SampleRateIdx);
	if (callback && ExtIOCallBack)
		ExtIOCallBack(-1, extHw_Changed_SampleRate, 0, NULL);// Signal application
}

static void updateGPIOCBs(HWND hwndDlg)
{
	Button_SetCheck(GetDlgItem(hwndDlg, IDC_GPIOA), gpioA ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(GetDlgItem(hwndDlg, IDC_GPIOB), gpioB ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(GetDlgItem(hwndDlg, IDC_GPIOC), gpioC ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(GetDlgItem(hwndDlg, IDC_GPIOD), gpioD ? BST_CHECKED : BST_UNCHECKED);
}

static void updateDevVerStrCB(HWND hwndDlg)
{
	_stprintf_s(dev_versionStrT, 127, TEXT("%S"), dev_versionStr);
	Static_SetText(GetDlgItem(hwndDlg, IDC_DEV_VERSTRING), dev_versionStrT);
}

static void updateLibVerStrCB(HWND hwndDlg)
{
	TCHAR tempStr[256];
	_stprintf_s(tempStr, 255, TEXT("%S"), airspyhf_lib_gitver);
	Static_SetText(GetDlgItem(hwndDlg, IDC_LIB_VERSTRING), tempStr);
}

static void updateFreqCorrCB(HWND hwndDlg)
{
	TCHAR tempStr[256];
	_stprintf_s(tempStr, 255, TEXT("%d"), FreqCorrPPB);
	Edit_SetText(GetDlgItem(hwndDlg, IDC_PPM), tempStr);
}

static void updateStatusCB(HWND hwndDlg)
{
	_stprintf_s(statusStrT, 255, TEXT("%S"), statusStr);
	Static_SetText(GetDlgItem(hwndDlg, IDC_STATUS), statusStrT);
}

static void setStatusCB(const char * text, bool bError)
{
	snprintf(statusStr, 255, "%s", text);
	_stprintf_s(statusStrT, 255, TEXT("%S"), statusStr);
	if (h_dialog)
	{
		Static_SetText(GetDlgItem(h_dialog, IDC_STATUS), statusStrT);
		if (bError)
			ShowGUI();
	}
	return;
}

static UDACCEL ppbAccel[] =
{
	{ 0, 1 }
	, { 1, 100 }
	, { 3, 1000 }
	, { 5, 10000 }
};


static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static HBRUSH BRUSH_RED=CreateSolidBrush(RGB(255,0,0));
	static HBRUSH BRUSH_GREEN=CreateSolidBrush(RGB(0,255,0));

	// see https://www.codeproject.com/Articles/831/Using-the-CEdit-control

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			SDRLOG(extHw_MSG_DEBUG, "Dialog: WM_INITDIALOG with %d ppb", FreqCorrPPB);
			SendMessage(GetDlgItem(hwndDlg, IDC_PPM_S), UDM_SETRANGE32, (WPARAM)MIN_PPB, (LPARAM)MAX_PPB);
			SendMessage(GetDlgItem(hwndDlg, IDC_PPM_S), UDM_SETACCEL, (WPARAM)3, (LPARAM)(&ppbAccel[0]) );

			updateDeviceListCB(hwndDlg);
			updateSampleRatesCB(hwndDlg, false);
			updateGPIOCBs(hwndDlg);
			updateDevVerStrCB(hwndDlg);
			updateLibVerStrCB(hwndDlg);
			updateFreqCorrCB(hwndDlg);
			updateStatusCB(hwndDlg);

			finishedInitDialog = true;
			return TRUE;
		}

		case WM_PRINT:
			if (lParam == (LPARAM)PRF_CLIENT)		// redraw client area
			{
				updateDeviceListCB(hwndDlg);
				updateSampleRatesCB(hwndDlg, false);
				updateGPIOCBs(hwndDlg);
				updateDevVerStrCB(hwndDlg);
				updateLibVerStrCB(hwndDlg);
				updateFreqCorrCB(hwndDlg);
				updateStatusCB(hwndDlg);
			}
			return TRUE;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDC_REFRESH_SRATES:	// refresh everything!
					updateDeviceListCB(hwndDlg);
					updateSampleRatesCB(hwndDlg, false);
					updateGPIOCBs(hwndDlg);
					updateDevVerStrCB(hwndDlg);
					updateLibVerStrCB(hwndDlg);
					updateFreqCorrCB(hwndDlg);
					updateStatusCB(hwndDlg);
					return TRUE;

				case IDC_PPM:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
					{
						TCHAR ppmT[256];
						TCHAR ppmEvalT[256];
						char ppmC[256];
						Edit_GetText((HWND) lParam, ppmT, 255 );
						snprintf(ppmC, 255, "%S", ppmT);
						int tempInt = finishedInitDialog ? _ttoi(ppmT) : FreqCorrPPB;
						if (MIN_PPB <= tempInt && tempInt <= MAX_PPB)
							FreqCorrPPB = tempInt;
						else if (tempInt < MIN_PPB)
							FreqCorrPPB = MIN_PPB;
						else if (tempInt > MAX_PPB)
							FreqCorrPPB = MAX_PPB;

						_stprintf_s(ppmEvalT, 255, TEXT("%d"), FreqCorrPPB);
						if (_tcscmp(ppmEvalT, ppmT))
							updateFreqCorrCB(hwndDlg);
						SDRLOG(extHw_MSG_DEBUG, "Dialog: ppb changed to '%s' --> %d ppb", ppmC, FreqCorrPPB);
						if (dev)
							airspyhf_set_calibration(dev, FreqCorrPPB);
					}
					return TRUE;
				case IDC_SAVE_PPB:
					if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
					{
						if (dev)
						{
							int is_streaming = airspyhf_is_streaming(dev);
							if (is_streaming)
							{
								// airspyhf_flash_calibration() requires stopped streaming!
								airspyhf_stop(dev);
								::Sleep(200);
							}

							int ret = airspyhf_flash_calibration(dev);
							if (ret != AIRSPYHF_SUCCESS)
								::MessageBoxA(hwndDlg, "Error writing ppb value to Flash", "Error", MB_OK);

							if (is_streaming)
								airspyhf_start(dev, airspyhf_sample_block_cb, NULL);
						}
						else
							::MessageBoxA(hwndDlg, "No device to Flash", "Error", MB_OK);
					}
					return TRUE;

				case IDC_DEV_SERIAL:
					if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
					{
						HWND hCtrl = GET_WM_COMMAND_HWND(wParam, lParam);
						int SerialIdx = ComboBox_GetCurSel(hCtrl);
						// HWND hDlgItmDevList = GetDlgItem(hwndDlg, IDC_DEV_SERIAL) == hCtrl
						changedSelectionSinceFailedOpen = true;
						if (0 <= SerialIdx && SerialIdx < num_combo_serials && !devStreaming)
						{
							const uint64_t prev_serial = sel_DeviceSerial;
							sel_DeviceSerial = combo_serials[SerialIdx];
							SDRLOG(extHw_MSG_DEBUG, "Dialog: SerialIdx %d -> '%s'", SerialIdx, Serial2SNtextC(sel_DeviceSerial));
							if (prev_serial != sel_DeviceSerial)
							{
								if (!devStreaming && dev)
								{
									SDRLOGTXT(extHw_MSG_DEBUG, "Dialog: --> closing for change");
									airspyhf_close(dev);
									dev = NULL;
									setStatusCB("Closed device.");
								}
								else
									setStatusCB("");
								snprintf(dev_versionStr, 127, "-");
							}
						}
						else
						{
							SDRLOG(extHw_MSG_DEBUG, "Dialog: SerialIdx %d -> %% : out of range or disabled!", SerialIdx);
						}
						updateDeviceListCB(hwndDlg, false);
						updateDevVerStrCB(hwndDlg);
					}
					return devStreaming ? FALSE : TRUE;

				case IDC_STATUS:
					//return TRUE;
					if (	// GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE ||
							GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE
						|| GET_WM_COMMAND_CMD(wParam, lParam) == WM_SETTEXT
						)
					{
						updateStatusCB(hwndDlg);
					}
					return TRUE;

				case IDC_SAMPLERATE:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    { 
						SampleRateIdx = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
						SDRLOG(extHw_MSG_DEBUG, "Dialog: SampleRate Idx changed to %d -> %u Hz", SampleRateIdx, combo_srates[SampleRateIdx]);
						updateSampleRatesCB(hwndDlg, true, false);	// callback, no ReFill
                    }
					return TRUE;
				case IDC_GPIOA:
					gpioA = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					SDRLOG(extHw_MSG_DEBUG, "Dialog: GPIO 0 is now %s", (gpioA ? "Hi" : "Lo"));
					if (dev)
						airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_0, (gpioA ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
					return TRUE;
				case IDC_GPIOB:
					gpioB = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					SDRLOG(extHw_MSG_DEBUG, "Dialog: GPIO 1 is now %s", (gpioB ? "Hi" : "Lo"));
					if (dev)
						airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_1, (gpioB ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
					return TRUE;
				case IDC_GPIOC:
					gpioC = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					SDRLOG(extHw_MSG_DEBUG, "Dialog: GPIO 2 is now %s", (gpioC ? "Hi" : "Lo"));
					if (dev)
						airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_2, (gpioC ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
					return TRUE;
				case IDC_GPIOD:
					gpioD = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					SDRLOG(extHw_MSG_DEBUG, "Dialog: GPIO 3 is now %s", (gpioD ? "Hi" : "Lo"));
					if (dev)
						airspyhf_set_user_output(dev, AIRSPYHF_USER_OUTPUT_3, (gpioD ? AIRSPYHF_USER_OUTPUT_HIGH : AIRSPYHF_USER_OUTPUT_LOW));
					return TRUE;
			}
            break;

		case WM_CLOSE:
			ShowWindow(hwndDlg, SW_HIDE);
            return TRUE;
			break;

		case WM_DESTROY:
			h_dialog=NULL;
			return TRUE;
			break;

		/*
		* TODO: Add more messages, when needed.
		*/
	}

	return FALSE;
}

