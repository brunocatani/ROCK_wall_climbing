# Changelog

## Unreleased

### Requirements

- Needs ROCK 1.0: the addon now binds ROCK's provider API 2.0. ROCK 0.92 and
  earlier are no longer supported. A loaded ROCK without the API this version
  was built for stops the game at the main menu with an error box naming the
  mismatch; without ROCK the addon stays off and the game runs.

### Changed

- Settings move to `ROCK_Wall_Climbing.toml` in
  `Documents\My Games\Fallout4VR\Mods_Config\ROCK_Wall_Climbing\`, with each
  option's help above it and new options added as later versions bring them.
  The first start carries every option you changed in `ROCK_Wall_Climbing.ini`
  into the TOML file and deletes the INI; nothing to do by hand.

Climbing itself works as in 1.1.
