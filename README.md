# Auto Clip Sorter

An OBS script that automatically organizes replay buffer saves into per-game folders based on the active capture source.

## Features

- **Smart sorting** — detects the game name from the window title or executable and routes replays to a matching folder.
- **Event-driven** — responds to OBS signals rather than polling, keeping CPU usage minimal.
- **Auto replay buffer** — optionally starts and stops the Replay Buffer when a game session begins or ends.

## Setup

1. In OBS, go to **Tools → Scripts** and click **+**.
2. Select `rb_event_driven.py`.
3. Configure the script:
   - **Source to monitor** — select your Game Capture or Window Capture source.
   - **Prefer capture hook signals** — improves accuracy by waiting for the hook signal.
   - **Organize clips** — enables folder sorting.
   - **Base output path** — root directory where per-game folders will be created.
