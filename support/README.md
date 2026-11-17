# NitLink support tools

Small helpers for when something doesn't work and I need to know what your PC sees.

## nitlink-device-info.ps1

Lists how Windows sees your capture card, and writes it to one text file on your Desktop.

**What it does**

- Reads only. It changes nothing on your PC.
- Needs no administrator rights.
- Sends nothing anywhere. You send the file yourself, after looking at it.

**What ends up in the file**

- Windows version
- Graphics card and driver version
- The video capture cards Windows knows, and how each one is registered
- Their driver versions
- NitLink's own log lines about capture devices, if NitLink is running

No sound devices, no user name, no other files.

**How to run it**

1. Start NitLink and leave it open.
2. Open the script and copy all of its text: [nitlink-device-info.ps1](nitlink-device-info.ps1) (use the "Raw" button, then select all and copy).
3. Right-click the Start button and choose **Terminal** (on Windows 10: **Windows PowerShell**).
4. Paste the text and press Enter. If Windows warns about pasting several lines, choose **Paste anyway**.
5. When it says "Done", open `NitLink-device-info.txt` on your Desktop, check it, and send it to support@nitlink.app or attach it where I asked for it.

The script is plain text. Read it before you run it if you like; it is about 100 lines.
