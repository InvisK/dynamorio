REM **********************************************************
REM Copyright (c) 2008 VMware, Inc.  All rights reserved.
REM **********************************************************
REM
REM Redistribution and use in source and binary forms, with or without
REM modification, are permitted provided that the following conditions are met:
REM 
REM * Redistributions of source code must retain the above copyright notice,
REM   this list of conditions and the following disclaimer.
REM 
REM * Redistributions in binary form must reproduce the above copyright notice,
REM   this list of conditions and the following disclaimer in the documentation
REM   and/or other materials provided with the distribution.
REM 
REM * Neither the name of VMware, Inc. nor the names of its contributors may be
REM   used to endorse or promote products derived from this software without
REM   specific prior written permission.
REM 
REM THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
REM AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
REM IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
REM ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
REM FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
REM DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
REM SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
REM CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
REM LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
REM OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
REM DAMAGE.

@echo off

set CUR_DIR=%CD%

set TARG=
if {%1}=={vmap} set TARG=VMAP
if {%1}=={viper} set TARG=VMSAFE
if {%TARG%}=={} goto usage

cd %~dp0\..
set DR_HOME=%CD%

call %DR_HOME%\batch\common.bat
if %errorlevel% neq 0 goto error

cd %DR_HOME%\core
%CYGBIN%\bash -c "PATH=%BASH_PATH% make clear"
%CYGBIN%\bash -c "PATH=%BASH_PATH% make %TARG%=1 DEBUG=0 INTERNAL=0 api_headers"

cd %DR_HOME%\api\docs
%CYGBIN%\bash -c "PATH=%BASH_PATH% make clean"
%CYGBIN%\bash -c "PATH=%BASH_PATH% make %TARG%=1 pdf"

REM clean up mounts
%CYGBIN%\umount -A

cd %CUR_DIR%
goto :eof

:usage
echo "usage: %0 <viper|vmap>"

:error
cd %CUR_DIR%
exit /b 1
