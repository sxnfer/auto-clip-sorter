# Install Clip Atlas

This guide covers installing Clip Atlas, the first start, updating, uninstalling, moving over
from the Python script, and fixing common problems.

You need:

- Windows 10 or 11, 64-bit.
- OBS Studio 31 or newer, 64-bit.
- An account that can approve a Windows permission prompt (the "admin prompt"). The installer
  and the zip both need it, because they install into `C:\ProgramData`.

Clip Atlas comes in two downloads on the
[Releases page](https://github.com/sxnfer/auto-clip-sorter/releases):

- **The installer**, `clip-atlas-<version>-windows-x64-installer.exe`. This is the recommended way.
- **The zip**, `clip-atlas-<version>-windows-x64.zip`, for a manual install.

Both put the same plugin in `C:\ProgramData\obs-studio\plugins\clip-atlas`. The Releases page
may also list Linux packages and source code. Clip Atlas is for Windows, so ignore those files.

## Install with the installer

1. Open the [Releases page](https://github.com/sxnfer/auto-clip-sorter/releases) and open the
   newest release. A release marked **Pre-release** is a test build.
2. Under **Assets**, download `clip-atlas-<version>-windows-x64-installer.exe`.
3. Close OBS Studio.
4. Double-click the installer. Windows asks whether to allow the app to make changes. Click
   **Yes**. If Windows first shows "Windows protected your PC", follow
   [the steps below](#windows-protected-your-pc).
5. Read the license, choose **I accept the agreement**, and click **Next**.
6. Setup does not ask for a folder. It installs to `C:\ProgramData\obs-studio\plugins\clip-atlas`.
   Click **Install**. If a message says OBS Studio is running, close OBS, then click **Retry**.
   Clicking **Cancel** stops the installer.
7. Click **Finish**. Setup does not start OBS, so start it yourself.

### Windows protected your PC

Windows may show a box titled **Windows protected your PC** when you run the installer. This
happens because Clip Atlas is not code-signed yet. A signing certificate tells Windows who
published a program. Code signing is planned for a later release, so until then Windows cannot
confirm the publisher.

To continue:

1. Click **More info**.
2. Check that the app name is `clip-atlas-<version>-windows-x64-installer.exe`, the file you
   downloaded, and that the publisher is **Unknown publisher**.
3. Click **Run anyway**.

If you would rather not run an installer, use the zip instead. It contains the same plugin.

Optional: the release notes list a SHA-256 checksum for each file. To check your download, run
`certutil -hashfile "<file>" SHA256` in Command Prompt, with `<file>` set to the path of the
downloaded file. The result should match the checksum in the release notes.

## Install from the zip

1. Open the [Releases page](https://github.com/sxnfer/auto-clip-sorter/releases) and download
   `clip-atlas-<version>-windows-x64.zip`.
2. Close OBS Studio.
3. Right-click the zip and choose **Extract All**. Open the new folder. Inside it is a folder
   called `clip-atlas`.
4. If `C:\ProgramData\obs-studio\plugins\clip-atlas` already exists, delete it first.
5. Copy the `clip-atlas` folder into `C:\ProgramData\obs-studio\plugins\`. Windows asks for
   administrator permission. Click **Yes**.

`C:\ProgramData` is a hidden folder. To open it, type `C:\ProgramData\obs-studio\plugins` into
the address bar of File Explorer.

When you finish, the file `C:\ProgramData\obs-studio\plugins\clip-atlas\bin\64bit\clip-atlas.dll`
should exist. The `clip-atlas` folder must sit directly inside `plugins`, not one level deeper.

## First start

1. Start OBS Studio.
2. Open **Help → Log Files → View Current Log** and search for `clip-atlas`. You should see a
   line that starts with `[clip-atlas] loaded`. If the log reports an error from Clip Atlas, see
   [Troubleshooting](#troubleshooting).
3. Open **Tools → Clip Atlas Settings…**. The **Clips folder** box is empty by default. The grey
   text in it, "Use OBS recording folder", means clips go to OBS's recording folder. To use a
   different folder, click **Browse…**. Click **OK** when you are done.
4. Make sure the scene you record or stream has a **Game Capture** source for your game.
5. For replays, turn on OBS's Replay Buffer. Open **Settings → Output → Replay Buffer**, tick
   **Enable Replay Buffer**, and click **OK**. Clip Atlas can then start the buffer for you when
   a game starts.
6. Open **Docks → Clip Atlas** to show the dock. OBS remembers where you place it.

## Updating

Clip Atlas checks for a newer version once a day, and **Tools → Check for Clip Atlas Updates…**
checks right away. The check looks only at full releases, not pre-releases.

If you installed with the installer, Clip Atlas updates itself:

1. When a newer version is out, the dock's banner says **Downloading Clip Atlas …**. The
   installer downloads in the background, and Clip Atlas checks it against the SHA-256 checksum
   GitHub lists for it. A file that doesn't match is thrown away.
2. The banner then says the update is ready and installs when you close OBS. **What's new**
   opens the release page.
3. Close OBS as usual. Windows asks whether to let the Clip Atlas installer make changes,
   because it writes to `%ProgramData%`. The publisher shows as unknown, since the installer is
   not code-signed yet. Choose **Yes**. A small progress window appears, then a message that
   the update is installed.
4. Start OBS Studio again.

If you choose **No** at Windows' prompt, nothing changes, and Clip Atlas offers the update again
after its next daily check. The download is kept in
`%APPDATA%\obs-studio\plugin_config\clip-atlas\updates\`, so it is not downloaded twice.

If you installed from the zip, or turned off **Download updates and install them when OBS
closes** under **Updates** in **Tools → Clip Atlas Settings…**, the banner's **Download…**
button opens the release page instead. You can turn the daily check off there too.

To update by hand:

1. Close OBS Studio.
2. If you installed with the installer, download the new installer and run it, as in
   [Install with the installer](#install-with-the-installer). If you installed from the zip,
   delete the old `clip-atlas` folder and copy in the new one, as in
   [Install from the zip](#install-from-the-zip).
3. Start OBS Studio.

Your settings are kept. They live in `%APPDATA%\obs-studio\plugin_config\clip-atlas\`, which the
installer does not touch.

## Uninstalling

1. Close OBS Studio. The uninstaller asks you to close it if it is running.
2. Open **Settings → Apps → Installed apps**. (On Windows 10, open
   **Settings → Apps → Apps & features**.) Find **Clip Atlas (OBS plugin)**, choose
   **Uninstall**, and follow the prompts.
3. If you installed from the zip, close OBS and delete the folder
   `C:\ProgramData\obs-studio\plugins\clip-atlas` instead.

Uninstalling does not delete your settings or your clips. To remove the settings too, delete
`%APPDATA%\obs-studio\plugin_config\clip-atlas` yourself.

## Coming from the Python script

The Python script `rb_event_driven.py` is deprecated. Remove it before you use Clip Atlas, because
running both would make them fight over the same clip.

1. In OBS, open **Tools → Scripts**.
2. Select `rb_event_driven.py` and click **−** to remove it.
3. Open **Tools → Clip Atlas Settings…** and set the **Clips folder** yourself. Clip Atlas does
   not import the script's settings yet (that is planned for milestone M4). If the script saved
   clips to a folder other than OBS's recording folder, enter that folder here.

This release does not warn you if the script is still loaded, so check **Tools → Scripts**
yourself.

## Troubleshooting

Start with the log. Open **Help → Log Files → View Current Log** and search for `clip-atlas`.
Most problems show up there. To send the log, see [Sending a log](#sending-a-log).

### The plugin does not appear

1. Check that `C:\ProgramData\obs-studio\plugins\clip-atlas\bin\64bit\clip-atlas.dll` exists. If
   it does not, install again, using one of the install sections above.
2. Check for an older copy in `%APPDATA%\obs-studio\plugins\clip-atlas`. If both copies exist,
   OBS loads both. The installer shows a warning when it finds this copy, but it never deletes
   it. Delete that folder yourself, then restart OBS.
3. Check that you use OBS Studio 31 or newer, 64-bit.
4. If the log has no `[clip-atlas] loaded` line, send the log.

### Clips go to the Unsorted folder

Clip Atlas needs to see the game through a capture source. Without one, clips go to Unsorted.

1. Add a **Game Capture** source for the game to the scene you record or stream.
2. Start the game and look at the dock header. It should show the game's name. If it says
   **No game detected**, Clip Atlas cannot see the game. Fix step 1 and try again.
3. To move a clip that is already in the Unsorted folder into a game's folder, right-click the clip
   in the dock and choose **Move to game…**.

### The Replay Buffer does not start

- Turn it on in OBS. Open **Settings → Output → Replay Buffer**, tick **Enable Replay Buffer**,
  and click **OK**. The dock shows a banner while the Replay Buffer is off in OBS.
- In **Tools → Clip Atlas Settings…**, check that **Start the Replay Buffer when a game is
  detected** is ticked.
- If you stopped the Replay Buffer yourself while the game was running, Clip Atlas does not start it
  again for that game. Click **Start Replay Buffer** in OBS to start it yourself.

### Clips stay in OBS's folder

- Check the **Auto-sort** switch in the dock header. When it is off, new clips stay where OBS saved
  them. Switch it on.
- Check **What to sort** in **Tools → Clip Atlas Settings…**. The type of clip must be ticked.
- If you closed OBS within a few seconds of stopping a recording or saving a replay, that file may
  be left unsorted in OBS's folder. If it is in the clips folder, it appears under **Not sorted** in
  the dock's game filter, and you can file it with **Move to game…**.

### The dock shows a banner about the clips folder

The banner says that the clips folder does not exist, and asks whether the drive is connected.
Connect the drive, or choose another folder with **Browse…** in
**Tools → Clip Atlas Settings…**.

### Thumbnails are missing

Clip Atlas takes its pictures from Windows, so Windows decides which clips have one.

- `.mp4` files usually get a thumbnail.
- `.mkv` files need a codec pack that gives Windows a thumbnail handler for MKV files.
- Without a thumbnail, the dock shows the file type's icon instead of a picture. The clip itself
  is fine.

### The save sound is heard on stream

This is expected. Windows plays the sound through its default playback device, and OBS's Desktop
Audio captures that device, so viewers and later clips can hear it. To stop it, untick
**Play a sound when a replay is saved** in **Tools → Clip Atlas Settings…**. Lowering the
**Volume** makes the sound quieter, but it is still captured.

### Sending a log

Choose one of these, and send the result to whoever is helping you:

- **Help → Log Files → Upload Current Log File**, then send the link it gives you.
- **Help → Log Files → View Current Log**. Search for `clip-atlas` and copy the lines around the
  problem.
