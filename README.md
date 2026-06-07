# roadblock-patcher

Allows to launch game with patched Eve URL address. Currently works with 24.6.1a.

Will be expanded in a future.

# Usage

* Create `eve_string` file in working directory.
  * It must contain Eve URL address, with `https://` at the beginning and no `/` at the end.
  * URL example: `https://test.example.com`.
* Launch executable with launch argument containing absolute path to game binary.
  * You can drag'n'drop game binary on patcher in Windows.
    * Make sure `eve_string` file is next to game binary then.
  * Or just create a shortcut for convenience.