# [Telegram Desktop][telegram_desktop] – Official Messenger

This is the complete source code and the build instructions for the official [Telegram][telegram] messenger desktop client, based on the [Telegram API][telegram_api] and the [MTProto][telegram_proto] secure protocol.

[![Version](https://badge.fury.io/gh/telegramdesktop%2Ftdesktop.svg)](https://github.com/telegramdesktop/tdesktop/releases)
[![Build Status](https://github.com/telegramdesktop/tdesktop/workflows/Windows./badge.svg)](https://github.com/telegramdesktop/tdesktop/actions)
[![Build Status](https://github.com/telegramdesktop/tdesktop/workflows/MacOS./badge.svg)](https://github.com/telegramdesktop/tdesktop/actions)
[![Build Status](https://github.com/telegramdesktop/tdesktop/workflows/Linux./badge.svg)](https://github.com/telegramdesktop/tdesktop/actions)
[![Built with Depot](https://img.shields.io/badge/Built%20with-Depot.dev-46A75A)](https://depot.dev)

[![Preview of Telegram Desktop][preview_image]][preview_image_url]

The source code is published under GPLv3 with OpenSSL exception, the license is available [here][license].

## Supported systems

The latest version is available for

* [Windows 7 and above (64 bit)](https://telegram.org/dl/desktop/win64) ([portable](https://telegram.org/dl/desktop/win64_portable))
* [Windows 7 and above (32 bit)](https://telegram.org/dl/desktop/win) ([portable](https://telegram.org/dl/desktop/win_portable))
* [macOS 10.13 and above](https://telegram.org/dl/desktop/mac)
* [Linux static build for 64 bit](https://telegram.org/dl/desktop/linux)
* [Snap](https://snapcraft.io/telegram-desktop)
* [Flatpak](https://flathub.org/apps/details/org.telegram.desktop)

## Old system versions

Version **4.9.9** was the last that supports older systems

* [macOS 10.12](https://updates.tdesktop.com/tmac/tsetup.4.9.9.dmg)
* [Linux with glibc < 2.28 static build](https://updates.tdesktop.com/tlinux/tsetup.4.9.9.tar.xz)

Version **2.4.4** was the last that supports older systems

* [OS X 10.10 and 10.11](https://updates.tdesktop.com/tosx/tsetup-osx.2.4.4.dmg)
* [Linux static build for 32 bit](https://updates.tdesktop.com/tlinux32/tsetup32.2.4.4.tar.xz)

Version **1.8.15** was the last that supports older systems

* [Windows XP and Vista](https://updates.tdesktop.com/tsetup/tsetup.1.8.15.exe) ([portable](https://updates.tdesktop.com/tsetup/tportable.1.8.15.zip))
* [OS X 10.8 and 10.9](https://updates.tdesktop.com/tmac/tsetup.1.8.15.dmg)
* [OS X 10.6 and 10.7](https://updates.tdesktop.com/tmac32/tsetup32.1.8.15.dmg)

## Third-party

* Qt 6 ([LGPL](http://doc.qt.io/qt-6/lgpl.html)) and Qt 5.15 ([LGPL](http://doc.qt.io/qt-5/lgpl.html)) slightly patched
* OpenSSL 3.2.1 ([Apache License 2.0](https://openssl-library.org/source/license/apache-license-2.0.txt))
* WebRTC ([New BSD License](https://github.com/desktop-app/tg_owt/blob/master/LICENSE))
* zlib ([zlib License](http://www.zlib.net/zlib_license.html))
* LZMA SDK 9.20 ([public domain](http://www.7-zip.org/sdk.html))
* liblzma ([public domain](http://tukaani.org/xz/))
* Google Breakpad ([License](https://chromium.googlesource.com/breakpad/breakpad/+/master/LICENSE))
* Google Crashpad ([Apache License 2.0](https://chromium.googlesource.com/crashpad/crashpad/+/master/LICENSE))
* GYP ([BSD License](https://github.com/bnoordhuis/gyp/blob/master/LICENSE))
* Ninja ([Apache License 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING))
* OpenAL Soft ([LGPL](https://github.com/kcat/openal-soft/blob/master/COPYING))
* Opus codec ([BSD License](http://www.opus-codec.org/license/))
* FFmpeg ([LGPL](https://www.ffmpeg.org/legal.html))
* Guideline Support Library ([MIT License](https://github.com/Microsoft/GSL/blob/master/LICENSE))
* Range-v3 ([Boost License](https://github.com/ericniebler/range-v3/blob/master/LICENSE.txt))
* Open Sans font ([Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0.html))
* Vazirmatn font ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))
* Emoji alpha codes ([MIT License](https://github.com/emojione/emojione/blob/master/extras/alpha-codes/LICENSE.md))
* xxHash ([BSD License](https://github.com/Cyan4973/xxHash/blob/dev/LICENSE))
* QR Code generator ([MIT License](https://github.com/nayuki/QR-Code-generator#license))
* CMake ([New BSD License](https://github.com/Kitware/CMake/blob/master/Copyright.txt))
* Hunspell ([LGPL](https://github.com/hunspell/hunspell/blob/master/COPYING.LESSER))
* Ada ([Apache License 2.0](https://github.com/ada-url/ada/blob/main/LICENSE-APACHE))
* zxcvbn-c ([MIT License](https://github.com/tsyrogit/zxcvbn-c/blob/master/LICENSE.txt))
* twidget ([BSD 3-Clause License](https://github.com/TelegramMessenger/twidget/blob/main/LICENSE))
* litehtml ([BSD 3-Clause License](https://github.com/litehtml/litehtml/blob/master/LICENSE))
* Gumbo ([Apache License 2.0](https://github.com/google/gumbo-parser/blob/master/COPYING))
* QuickJS-ng ([MIT License](https://github.com/quickjs-ng/quickjs/blob/master/LICENSE))
* FrequencyWords by Hermit Dave ([Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/))

## Build instructions

* [Windows (32-bit and 64-bit)][win]
* [macOS][mac]
* [GNU/Linux using Docker][linux]

[//]: # (LINKS)
[telegram]: https://telegram.org
[telegram_desktop]: https://desktop.telegram.org
[telegram_api]: https://core.telegram.org
[telegram_proto]: https://core.telegram.org/mtproto
[license]: LICENSE
[win]: docs/building-win.md
[mac]: docs/building-mac.md
[linux]: docs/building-linux.md
[preview_image]: https://github.com/telegramdesktop/tdesktop/blob/dev/docs/assets/preview.png "Preview of Telegram Desktop"
[preview_image_url]: https://raw.githubusercontent.com/telegramdesktop/tdesktop/dev/docs/assets/preview.png

## Thanks to

<a href="https://depot.dev">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://depot.dev/assets/brand/1693758816/depot-logo-horizontal-on-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="https://depot.dev/assets/brand/1693758816/depot-logo-horizontal-on-light.svg">
    <img alt="Depot" src="https://depot.dev/assets/brand/1693758816/depot-logo-horizontal-on-light.svg" width="150">
  </picture>
</a>

CI infrastructure sponsored by [Depot](https://depot.dev) — fast GitHub Actions runners.

[![Github All Releases](https://img.shields.io/github/downloads/Forkgram/tdesktop/total.svg?style=for-the-badge)](https://github.com/Forkgram/tdesktop/releases)  
[![Snap Store](https://img.shields.io/snapcraft/v/forkgram/latest/stable?style=for-the-badge)](https://snapcraft.io/forkgram)  
[![AUR](https://img.shields.io/aur/version/forkgram.svg?style=for-the-badge)](https://aur.archlinux.org/packages/forkgram)  
[![Chocolatey](https://img.shields.io/chocolatey/v/forkgram.svg?style=for-the-badge)](https://community.chocolatey.org/packages/forkgram)  
[![Scoop](https://img.shields.io/scoop/v/forkgram?bucket=extras&style=for-the-badge)](https://scoop.sh/#/apps?q=forkgram)
[![Windows](https://img.shields.io/badge/Windows-x64%20%7C%20x86-blue?style=for-the-badge&logo=windows)](https://github.com/Forkgram/tdesktop/releases/latest)  
[![Homebrew Cask](https://img.shields.io/homebrew/cask/v/forkgram.svg?style=for-the-badge)](https://formulae.brew.sh/cask/forkgram)  
[![macOS](https://img.shields.io/badge/macOS-arm64%20%7C%20x86-black?style=for-the-badge&logo=apple)](https://github.com/Forkgram/tdesktop/releases/latest)  

## Features:
- Square avatars. *(Added XX.04.2017.)*  
Square tray icon. *(Updated 18.08.2018.)*

- Opening any links with a custom URI Scheme. *(Added XX.04.2017 and updated 02.08.2019.)*  

- Checkmark "Delete for everyone" by default. *(Added XX.04.2017.)*  
![image](https://user-images.githubusercontent.com/4051126/43633718-bee9dd4a-9712-11e8-8b24-57822bd20c04.png)

- The fourth button in the title to "Always on top" window. (Windows only.) *(Added 16.05.2017.)*  
![image](https://user-images.githubusercontent.com/4051126/43633763-d441eaa2-9712-11e8-8fb1-1e9e37af1d5e.png)

- Showing the full timestamp if the user was online more than 1 hour ago. *(Added 14.06.2017.)*  
![image](https://user-images.githubusercontent.com/4051126/27127613-5ead16c2-5104-11e7-8be0-f231b4bc3f6b.png)

- Hotkeys for the mediaview was usability improved. [**Cheatsheet**](#cheatsheet). *(Added 29.06.2017.)*  

- Experimental auto-submitting of the local passcode. *(Added 29.06.2017.)*  
![2018-08-10_01-50-45](https://user-images.githubusercontent.com/4051126/43930001-e5d1c1e8-9c3f-11e8-9ddd-ff1a60518d91.gif)

- Showing date of forwarded messages in the title. *(Added 25.08.2017.)*  
![image](https://user-images.githubusercontent.com/4051126/30046488-c94deb14-9213-11e7-8b2b-397ad6dbe6f7.png)

- Button to go to the first message. *(Added 16.05.2018.)* ([Telegrate's](https://github.com/Sea-n/tdesktop) idea.)  
![image](https://user-images.githubusercontent.com/4051126/40266489-7a31a1c8-5b54-11e8-868e-3c9877e0d6ff.png)

- Option to remove fading of audio when it's paused/played. *(Added 10.08.2018.)*  

- Ability to see thumbs of the executable files as well as for other files. *(Added 14.08.2018.)*  

- Ability to search a selected text via the context menu. *(Added 22.01.2019.)*  

- Ability to show the last seen time of the user in list of the dialogs. *(Added 31.01.2019.)*  

- Option to enable a display of all recently used stickers. *(Added 09.02.2019.)*  

- Auto-replacing of "..." with "…". *(Added 17.02.2019.)*  

- Ability to bind Ctrl+6-9 dialogs with Ctrl+Shift+6-9 shortcuts. *(Added 11.06.2019.)*  

- Ability to send files to Saved Messages without the forward header. *(Added 23.06.2019.)*  

- Forward messages without quoting the original sender via `Share` box. *(Added 25.06.2019.)*  



## Cheatsheet

| Zoom in           | Zoom out           | Zoom reset         | Old zoom in           | Old zoom out           | Old zoom reset         |
|----------------|-----------------|------------------|----------------|-----------------|------------------|
| +     | -             | 0                | Ctrl`+`+     | Ctrl`+`-             | Ctrl`+`0                |
| Up arrow     | Down arrow               |  *               | Ctrl`+`]     | Ctrl`+`_               |  Middle button              |
| Scroll up     | Scroll down              | Middle button                 | Ctrl`+`=     | Ctrl`+`Scroll down              |                    |
| ]     | _             |                  | Ctrl`+`*     |                 |                  |
| =     |              |                  | Ctrl`+`Scroll up         |              |                  |

You can scroll images by the wheel with pressed Ctrl.

## Downloads:
You can download binaries from the Releases or from my [Telegram channel Forkgram](https://t.me/forkgram).
