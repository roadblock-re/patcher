# roadblock-patcher

Allows to launch game with patched Eve URL address and window title. 

Currently works with 24.0.1f, 24.6.1a and 47.1.0a.

Will be expanded in a future.

# Usage

* Create `eve_string` file in working directory.
  * It must contain Eve URL address, with `https://` at the beginning and no `/` at the end.
  * URL example: `https://eve.example.com`.
* **Optional:** Create `window_title` file in working directory.
  * It must contain window title string pattern. First `{}` are replaced with game version. Second `{}` are replaced with current graphics API.
  * Title example: `Roadblock // {} // {}`.
* Launch executable with launch argument containing absolute path to game binary.
  * You can drag'n'drop game binary on patcher in Windows.
    * Make sure `eve_string` and `window_title` files are next to game binary then.
  * Or just create a shortcut for convenience.