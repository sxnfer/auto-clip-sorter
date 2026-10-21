# DEPRECATED: this script is superseded by the Clip Atlas native OBS plugin.
# It still works but receives no new features. See legacy/README.md.
import obspython as obs
import os
import shutil
from pathlib import Path

source_name = ""
source_ref = None
current_game_name = ""
wired_signals = []
prefer_hook_signals = True
_ui_refresh_scheduled = False

_connect_retry_cb = None
_connect_retry_attempt = 0
_CONNECT_MAX_ATTEMPTS = 6
_CONNECT_MAX_DELAY_MS = 2000

base_output_path = ""
create_source_folders = True

_DEPRECATION_NOTICE = (
    "Auto Clip Sorter (Python) is deprecated and replaced by the Clip Atlas plugin. "
    "Remove this script before installing Clip Atlas so clips are not moved twice."
)

def _sanitize_folder_name(name: str) -> str:
    """Sanitize the folder name by removing invalid characters."""
    invalid_chars = '<>:"/\\|?*'
    for char in invalid_chars:
        name = name.replace(char, '_')
    name = name.strip('. ')
    return name[:100] if name else "Unknown_Source"

def _get_source_folder_path() -> str:
    """Get the full path for the current source's folder."""
    if not base_output_path:
        return ""
    
    name_to_use = current_game_name if current_game_name else source_name
    if not name_to_use:
        return ""

    return os.path.join(base_output_path, _sanitize_folder_name(name_to_use))

def _move_clip_to_source_folder(original_path: str):
    """Move the saved replay clip to the source-specific folder."""
    if not create_source_folders or not source_name or not os.path.exists(original_path):
        return
    
    folder_path = _get_source_folder_path()
    if not folder_path:
        return
    
    try:
        Path(folder_path).mkdir(parents=True, exist_ok=True)
        
        filename = os.path.basename(original_path)
        destination = os.path.join(folder_path, filename)
        
        if os.path.exists(destination):
            base, ext = os.path.splitext(filename)
            counter = 1
            while os.path.exists(destination):
                destination = os.path.join(folder_path, f"{base}_{counter}{ext}")
                counter += 1
        
        shutil.move(original_path, destination)
        obs.script_log(obs.LOG_INFO, f"Moved clip to: {destination}")
    except Exception as e:
        obs.script_log(obs.LOG_ERROR, f"Failed to move clip '{original_path}': {e}")

def _get_last_replay_path() -> str:
    """Retrieve the path of the last saved replay."""
    try:
        path = obs.obs_frontend_get_last_replay()
        return path if path else ""
    except Exception as e:
        obs.script_log(obs.LOG_ERROR, f"Error getting replay path: {e}")
        return ""

def _set_rb(active: bool):
    """Start or stop the Replay Buffer."""
    def _apply():
        obs.timer_remove(_apply)
        is_active = obs.obs_frontend_replay_buffer_active()
        if active and not is_active:
            obs.obs_frontend_replay_buffer_start()
            obs.script_log(obs.LOG_INFO, "Replay Buffer: start requested")
        elif not active and is_active:
            obs.obs_frontend_replay_buffer_stop()
            obs.script_log(obs.LOG_INFO, "Replay Buffer: stop requested")
        _schedule_ui_refresh()

    obs.timer_add(_apply, 1)

def _ui_refresh_tick():
    """Reset the UI refresh scheduled flag."""
    global _ui_refresh_scheduled
    obs.timer_remove(_ui_refresh_tick)
    _ui_refresh_scheduled = False

def _schedule_ui_refresh():
    """Schedule a UI refresh to update button states."""
    global _ui_refresh_scheduled
    if not _ui_refresh_scheduled:
        _ui_refresh_scheduled = True
        obs.timer_add(_ui_refresh_tick, 100)

def _detect_game_name(calldata=None):
    """Detect the name of the game being captured from signal data or settings."""
    global current_game_name
    new_name = ""

    if calldata:
        title = obs.calldata_string(calldata, "title")
        exe = obs.calldata_string(calldata, "executable")
        new_name = title or exe

    if not new_name and source_ref:
        settings = obs.obs_source_get_settings(source_ref)
        if settings:
            window_str = obs.obs_data_get_string(settings, "window")
            obs.obs_data_release(settings)
            if window_str:
                parts = window_str.split("]:", 1)
                new_name = parts[1].strip() if len(parts) > 1 else window_str

    if new_name:
        current_game_name = new_name
        obs.script_log(obs.LOG_INFO, f"Detected game: {current_game_name}")

def _make_signal_cb(sig: str, start: bool):
    """Create a callback for a source signal."""
    def _cb(cd):
        obs.script_log(obs.LOG_DEBUG, f"Signal: {sig} for '{source_name}'")
        
        if sig == "hooked":
            _detect_game_name(cd)
        elif sig == "unhooked":
            global current_game_name
            current_game_name = ""
        elif sig == "activate" and not current_game_name:
            _detect_game_name()

        _set_rb(start)

    return _cb

def _disconnect_current():
    """Disconnect from the current source."""
    global source_ref, wired_signals, _connect_retry_cb, _connect_retry_attempt, current_game_name
    
    if source_ref is not None:
        sh = obs.obs_source_get_signal_handler(source_ref)
        for sig, cb in wired_signals:
            obs.signal_handler_disconnect(sh, sig, cb)
        wired_signals = []
        obs.obs_source_release(source_ref)
        source_ref = None
        current_game_name = ""
        obs.script_log(obs.LOG_INFO, "Disconnected from previous source")
    
    if _connect_retry_cb is not None:
        obs.timer_remove(_connect_retry_cb)
        _connect_retry_cb = None
        _connect_retry_attempt = 0

def _connect_to_source(name: str):
    """Connect to a new source."""
    global source_ref, wired_signals, _connect_retry_cb, _connect_retry_attempt
    
    _disconnect_current()
    if not name:
        return

    src = obs.obs_get_source_by_name(name)
    if not src:
        obs.script_log(obs.LOG_DEBUG, f"Source '{name}' not found yet; retrying…")
        _schedule_connect_retry()
        return

    if _connect_retry_cb:
        obs.timer_remove(_connect_retry_cb)
        _connect_retry_cb = None
        _connect_retry_attempt = 0

    source_ref = src
    obs.script_log(obs.LOG_INFO, f"Connecting to source: '{name}'")
    
    sh = obs.obs_source_get_signal_handler(src)
    signals = [
        ("activate", True), ("deactivate", False),
        ("show", True), ("hide", False)
    ]
    if prefer_hook_signals:
        signals.extend([("hooked", True), ("unhooked", False)])

    for sig, start in signals:
        cb = _make_signal_cb(sig, start)
        obs.signal_handler_connect(sh, sig, cb)
        wired_signals.append((sig, cb))

    _detect_game_name()
    w, h = obs.obs_source_get_width(src), obs.obs_source_get_height(src)
    _set_rb(w > 0 and h > 0)

def _schedule_connect_retry():
    """Schedule a retry for connecting to the source."""
    global _connect_retry_cb, _connect_retry_attempt
    if not source_name or _connect_retry_attempt >= _CONNECT_MAX_ATTEMPTS:
        return

    _connect_retry_attempt += 1
    delay = min(200 * (2 ** (_connect_retry_attempt - 1)), _CONNECT_MAX_DELAY_MS)

    def fire():
        global _connect_retry_cb
        obs.timer_remove(fire)
        _connect_retry_cb = None
        _connect_to_source(source_name)

    if _connect_retry_cb:
        obs.timer_remove(_connect_retry_cb)
    _connect_retry_cb = fire
    obs.timer_add(fire, delay)

def _handle_replay_saved():
    """Handle the replay saved event."""
    if not create_source_folders or not source_name:
        return
    
    def _process_saved_file():
        obs.timer_remove(_process_saved_file)
        replay_path = _get_last_replay_path()
        if replay_path and os.path.exists(replay_path):
            _move_clip_to_source_folder(replay_path)
        else:
            obs.script_log(obs.LOG_WARNING, "Could not determine replay file path")
    
    obs.timer_add(_process_saved_file, 500)

def _on_frontend_event(event):
    """Handle OBS frontend events."""
    if event in (obs.OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED, obs.OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED):
        _schedule_ui_refresh()
    elif event == obs.OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
        _handle_replay_saved()
    elif event in (getattr(obs, "OBS_FRONTEND_EVENT_FINISHED_LOADING", -1),
                   getattr(obs, "OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED", -2)):
        if source_name and source_ref is None:
            _schedule_connect_retry()

def script_description() -> str:
    """Return the script description."""
    return (
        "<b>Deprecated:</b> superseded by the Clip Atlas native plugin. "
        "Remove this script before installing Clip Atlas.<br><br>"
        "Event-driven Replay Buffer controller (no polling).\n"
        "Starts RB when the selected source is active/hooked, stops when it deactivates.\n"
        "Automatically organizes saved clips into folders by source name."
    )

def script_properties() -> object:
    """Define script properties."""
    props = obs.obs_properties_create()
    
    p_list = obs.obs_properties_add_list(
        props, "source_name", "Source to monitor",
        obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING
    )
    
    obs.obs_property_list_clear(p_list)
    sources = obs.obs_enum_sources()
    if sources:
        for src in sources:
            name = obs.obs_source_get_name(src)
            obs.obs_property_list_add_string(p_list, name, name)
        obs.source_list_release(sources)
    
    obs.obs_properties_add_bool(props, "prefer_hook_signals", "Prefer capture hook signals")
    obs.obs_properties_add_button(props, "refresh_sources", "Refresh source list", 
                                  lambda p, _: (script_properties(), True)[1])

    obs.obs_properties_add_bool(props, "create_source_folders", "Organize clips into source-named folders")
    obs.obs_properties_add_path(props, "base_output_path", "Base folder for organized clips",
                                obs.OBS_PATH_DIRECTORY, "", "")
    obs.obs_properties_add_text(props, "info_text", 
                                "Info: Clips will be organized into subfolders named after the monitored source.",
                                obs.OBS_TEXT_INFO)
    return props

def script_defaults(settings):
    """Set default settings."""
    obs.obs_data_set_default_bool(settings, "prefer_hook_signals", True)
    obs.obs_data_set_default_bool(settings, "create_source_folders", True)
    
    output = obs.obs_frontend_get_replay_buffer_output()
    if output:
        s = obs.obs_output_get_settings(output)
        if s:
            path = obs.obs_data_get_string(s, "path")
            if path:
                obs.obs_data_set_default_string(settings, "base_output_path", path)
            obs.obs_data_release(s)
        obs.obs_output_release(output)

def script_update(settings):
    """Update script settings."""
    global source_name, prefer_hook_signals, base_output_path, create_source_folders
    
    new_name = obs.obs_data_get_string(settings, "source_name")
    prefer_hook_signals = obs.obs_data_get_bool(settings, "prefer_hook_signals")
    create_source_folders = obs.obs_data_get_bool(settings, "create_source_folders")
    base_output_path = obs.obs_data_get_string(settings, "base_output_path")

    if new_name != source_name:
        source_name = new_name
        _connect_to_source(source_name)
    elif source_name and source_ref is None:
        _schedule_connect_retry()

def script_load(settings):
    """Load the script."""
    obs.script_log(obs.LOG_WARNING, _DEPRECATION_NOTICE)
    obs.obs_frontend_add_event_callback(_on_frontend_event)
    _schedule_connect_retry()

def script_unload():
    """Unload the script."""
    _disconnect_current()