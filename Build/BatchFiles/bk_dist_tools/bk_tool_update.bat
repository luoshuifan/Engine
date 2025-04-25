@echo off

rem download install.bat
set ver=5.5
set workdir=%TMP%\.bk_dist
bk-help-tool.exe download --url "http://devgw.devops.oa.com/turbo-client/disttask/install_ue4.bat" --file %workdir%\install.bat

call %workdir%\install.bat "%cd%" %ver%