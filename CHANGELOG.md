# Changelog

## 1.2.0

### Requirements

- Needs ROCK 1.0.0: the addon now binds ROCK's provider API 2.0. ROCK 0.92 and
  earlier are no longer supported. A loaded ROCK without the API this version
  was built for stops the game at the main menu with an error box naming the
  mismatch; without ROCK the addon stays off and the game runs.

### Added

- Launches fly. Letting go while pulling now carries you up, across gaps and
  along walls until you land; before, the launch was lost within a few frames
  and you dropped where you let go.
- Catch a wall in the air: a hand whose grip is held grabs a surface the
  moment it touches it. A grip pressed just before the hand lands on the wall
  grabs too. `CatchWithHeldGrip` in `[Climbing]` turns it off.
- Ledge vault: when you let go, or land from a launch, with your head above a
  ledge and your feet below it, you are lifted onto the ledge.

### Changed

- Launches are stronger: the strength default goes from 1.15 to 1.25 and the
  top speed from 450 to 600. They are now `Strength` and
  `TopSpeedGameUnitsPerSecond` in `[Launch]`; `Multiplier` and
  `MaximumSpeedGameUnitsPerSecond` are no longer read. The new
  `GravityGameUnitsPerSecondSquared` sets how fast a launch falls back down.
- Settings move to `ROCK_Wall_Climbing.toml` in
  `Documents\My Games\Fallout4VR\Mods_Config\ROCK_Wall_Climbing\`, with each
  option's help above it and new options added as later versions bring them.
  The first start carries every option you changed in `ROCK_Wall_Climbing.ini`
  into the TOML file and deletes the INI; nothing to do by hand.
