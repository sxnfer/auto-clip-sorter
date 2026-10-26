# Manual test checklist: updates that install when OBS closes

**What this adds:** when a newer release is out, Clip Atlas downloads its installer in the background, checks it
against the SHA-256 checksum GitHub lists for it, and runs it after you close OBS. Windows asks for permission first.
A copy installed from the zip keeps the old **Download…** button, which opens the release page.

**You need:** Windows 10 or 11, OBS Studio 31 or newer, and a build of this change whose version is **older** than the
latest published release, so the check finds an update. The test build is versioned 1.1.1; the latest release is
1.1.2, so the update it installs is 1.1.2 (which doesn't have this feature yet, so steps 9–12 run on the test build).

Write **Pass** or **Fail** in each Result cell. If something fails, open **Help → Log Files → View Current Log**,
search for `clip-atlas`, and paste the nearby lines into Notes.

## Steps

| # | Step | Expected result | Result | Notes |
|---|---|---|---|---|
| 1 | Close OBS. Install the 1.1.1 test build with its installer. Delete `%APPDATA%\obs-studio\plugin_config\clip-atlas\update-check.json` so the daily check runs at once. | It installs. | | |
| 2 | Start OBS. Open **Tools → Clip Atlas Settings…**. | Under **Updates** there are two ticked boxes: **Check for updates once a day** and **Download updates and install them when OBS closes**, and a line saying how updates install. | | |
| 3 | Wait about a minute. | The dock's banner says **Downloading Clip Atlas 1.1.2…**, then **Clip Atlas 1.1.2 is ready. It installs when you close OBS.** with a **What's new** button. The log says "updates can be installed when OBS closes", "downloading the 1.1.2 installer" and "the 1.1.2 installer is downloaded and checked". | | |
| 4 | Click **What's new**. | The 1.1.2 release page opens in your browser. | | |
| 5 | Look in `%APPDATA%\obs-studio\plugin_config\clip-atlas\updates\`. | It holds `clip-atlas-1.1.2-windows-x64-installer.exe` and no `.part` file. | | |
| 6 | Close OBS. | Windows asks whether to let the Clip Atlas installer make changes (unknown publisher). Choose **Yes**. A small progress window appears once OBS has gone, then "Clip Atlas 1.1.2 is installed". No wizard pages, no licence page. | | |
| 7 | Start OBS and check the log. | `[clip-atlas] loaded v1.1.2`. Your settings are kept. | | |
| 8 | Install the 1.1.1 test build again. Delete `update-check.json`, start OBS, wait for **ready**, then close OBS and choose **No** at Windows' prompt. | Nothing is installed. The next start still loads v1.1.1. The log of the earlier session ends with "the 1.1.2 installer did not start: the Windows permission prompt was declined". | | |
| 9 | Delete `update-check.json`. Untick **Download updates and install them when OBS closes**, click OK, restart OBS and wait a minute. | The banner says Clip Atlas 1.1.2 is available, with **Download…**, which opens the release page. Nothing is downloaded. Tick the box again afterwards. | | |
| 10 | Delete `update-check.json`. Start OBS, wait for **ready**, then untick the box, click OK, and close OBS. | No Windows prompt; nothing is installed. The log says "an update is ready, but installing on exit is off". Tick it again. | | |
| 11 | Delete `update-check.json`. Click **Tools → Check for Clip Atlas Updates…** right after starting OBS. | The box asks "Download it now? It installs when you close OBS." **Yes** shows the downloading, then ready, banner, and no browser page opens. | | |
| 12 | Delete `update-check.json`. Disconnect the network, start OBS, and reconnect it after the "Downloading" banner appears (or start the download and pull the cable during it). | Within a few minutes the banner falls back to **Download…** (the release page). The log has one WARNING saying why. No `.part` file is left in the `updates` folder. | | |

## Known limits (expected, not failures)

- The checksum check catches a broken or cut-off download. It does not prove who built the installer; only code
  signing does that, and the installer is not signed yet.
- If you close OBS while an update downloads, nothing is installed that time. The download starts again at the next
  daily check.
- If you choose **No** at Windows' prompt, the update is offered again at the next daily check. The download is kept,
  so it is not downloaded twice.
- A copy of Clip Atlas unpacked from the zip never installs updates itself.
