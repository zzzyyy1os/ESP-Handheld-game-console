@echo off
set IDF_PATH=D:\esp\v5.5.4\esp-idf
set PATH=C:\Users\35063\.espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1;C:\Espressif\tools\python;%PATH%
cd /d E:\esp\espnow-1
python D:\esp\v5.5.4\esp-idf\tools\idf.py build > build_log.txt 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL%
