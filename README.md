# Auto Clip Sorter 🎬

Yo! Welcome to the **Auto Clip Sorter**. This is a chill little OBS script that helps you keep your replays organized without you having to lift a finger.

## What's this?

Basically, if you're tired of having all your clips dumped into one giant folder, this script is for you. It watches what game or window you're capturing and automatically sorts your saved replays into folders named after that game.

**Features:**
*   **Smart Sorting:** Detects the game name (from the window title or executable) and creates a folder for it.
*   **Event-Driven:** No constant polling that eats up your CPU. It waits for OBS to tell it something happened.
*   **Auto Replay Buffer:** Can automatically turn on the Replay Buffer when you launch a game and turn it off when you quit. (Optional, but super handy).

## How to use

1.  Drop `rb_event_driven.py` into your OBS scripts folder.
2.  In OBS, go to **Tools -> Scripts**.
3.  Click the **+** button and select `rb_event_driven.py`.
4.  **Configure it:**
    *   **Source to monitor:** Pick your Game Capture or Window Capture source.
    *   **Prefer capture hook signals:** Keeps things accurate by waiting for the hook.
    *   **Organize clips:** Check this to enable the folder sorting magic.
    *   **Base output path:** Where you want your sorted folders to live.

That's it! Now go clip some crazy moments and let the script handle the cleanup. 🚀
