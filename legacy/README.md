# Auto Clip Sorter (Python script) — deprecated

`rb_event_driven.py` is the original OBS Python script this project started as. It is
**deprecated** and superseded by the **Clip Atlas** native plugin, which needs no Python
install and adds a clip browser dock, recordings/screenshot sorting, and game name rules.

The script still works but will not receive new features or fixes.

## Migrating to Clip Atlas

1. In OBS, open **Tools → Scripts**, select `rb_event_driven.py`, and click **−** to remove it.
   Running both would make them fight over the same clip.
2. Install Clip Atlas.
3. Optional (coming in v1): in Clip Atlas settings, use **Advanced → Import settings from the Python script**
   to carry over your output folder.

## Using the script anyway

Requires Python 3 (64-bit) configured under **Tools → Scripts → Python Settings**.

1. **Tools → Scripts → +**, select `legacy/rb_event_driven.py`.
2. Choose the source to monitor and the base output folder.
