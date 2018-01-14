# ExtIO_AirSpyHFplus
Winrad/HDSDR plugin ExtIO for the AirSpyHF+ receiver

Step-by-step installation

* install your favorite SDR software, e.g. HDSDR from http://www.hdsdr.de/ .
Other ExtIO compatible software like Winrad or Studio1 should also work.

* download ExtIO_AirSpyHFplus.DLL https://github.com/hayguen/ExtIO_AirSpyHFplus/releases

* copy downloaded file into your SDR software's installation directory (default=C:\Program Files (x86)\HDSDR).
Do NOT try to unzip directly into this directory! Because of Windows' user rights management this will fail. Unzip somewhere else first, then copy it to the installation directory.

* exit and restart SDR software and select ExtIO_AirSpyHFplus.DLL if demanded

* with HDSDR, you can use multiple devices by renaming the DLL filename, e.g. into ExtIO_AirSpyHFplus_HF.DLL or ExtIO_AirSpyHFplus_VHF.DLL. In addition, you might use different HDSDR profiles to see which device/profile is used in HDSDR's window title. See https://sites.google.com/site/g4zfqradio/installing-and-using-hdsdr#Advanced

* for AirSpy HF+, see https://airspy.com/airspy-hf-plus/
