# Racing wheels, pedals, H-shifters and handbrakes

Any wheel Windows sees as a game controller (Logitech, Thrustmaster, Fanatec, Moza, Simagic, ...)
is presented to the game as the **Xbox 360 Wireless Racing Wheel**. Pedals, an H-pattern or
sequential shifter and an analog handbrake can be part of the wheel or **separate USB devices**:
they are read together with the wheel as one controller. Everything is set up in the launcher's
**Wheel stuff** tab. The logic is ported from Forza Horizon 1: ReImagined's wheel driver
([NOTICE-wheel.md](../NOTICE-wheel.md)); devices are read with DirectInput (`src/input/wheel`).

> **Status: tested on one wheel.** Played in FM4 with a Simagic Alpha EVO wheelbase: the game takes
> it for an Xbox 360 wheel, its bindings work, and so does the force feedback (both playbacks, the
> rumble and the minimum force). Other wheels, and pedals, shifters and handbrakes on their own USB
> devices, are untested. How the game reads the wheel follows what FH1 traced in the Forza engine (FH1
> is built on FM4's engine): SubType 2 in the capabilities, 900 in their `thumb_lx` for full-range
> steering, the H-pattern's gear on the right stick X, the clutch on the right stick Y and the analog
> handbrake on the left stick Y. Please report what works with your wheel (and the log's `Wheel:`
> lines).

## The Wheel stuff tab

- **Devices**: every wheel, pedal set, shifter and handbrake, with what each is used as (the wheel,
  pedals / shifter / handbrake read with the wheel, or another wheel, a second controller). Click
  one for its live inputs: axes as bars, buttons lit while pressed, hats. **Use as the wheel** picks
  the first player's wheel; **Choose the wheel automatically** goes back to the default (a device
  with force feedback first, then one Windows lists as a wheel).
- **What the game gets**: steering, throttle, brake, clutch, handbrake, the gear sent to the game
  and the pad buttons, live, as the game would get them.
- **Bindings**: each control has a row with what it reads now (or *default*: the built-in or
  detected input). **Bind**, then turn, press or pull the control on **any** device:
  - a pedal pressed on a pedal set makes that the pedal device (`wheel_pedal_device`, its axis in
    `wheel_pedal_axes`); pressed on the wheel it reads the wheel (`wheel_axes`);
  - a gear put in on a shifter makes that the shifter device (`wheel_shifter_device`,
    `wheel_shifter_device_buttons`); on the wheel it reads the wheel's buttons
    (`wheel_shifter_buttons`). Binding a gear switches the shifter to H-pattern (sequential for its
    up and down rows) if it was off;
  - a handbrake pulled on its own device makes that the handbrake device
    (`wheel_handbrake_device`, `wheel_handbrake_axis`); on the wheel `wheel_axes` `handbrake=N`;
  - the steering axis: the device you turn becomes the wheel;
  - buttons are read on the wheel. The direction you move a control sets its invert switch.

  **Clear** unbinds a row, **Reset** gives it its default back, **Reset all** drops every binding and
  device choice. Inputs two rows share are listed in orange. Esc cancels a Bind.
- **Options**: steering mode (**900° full range** or the 360 wheel's **270° mode**), wheel rotation
  and steering lock, deadzone, the 270 degree mode's steering offset, inverting; separate or
  combined pedals, clutch pedal on or off, pedal curves; the shifter (off, H-pattern, sequential),
  realistic shifting and its clutch threshold, whether the gear follows the lever's slot; inverting
  the handbrake; which devices are the pedals, shifter and handbrake (auto, none, or a device).

The settings are saved to `Documents\fm4\settings.ini` straight away and apply the next time the
game starts. The live view runs `fm4.exe --wheel-probe` (the game's own wheel code, nothing of the
game loaded) while the tab is open; `FM4_WHEEL_PROBE` can name the standalone build from
`tools/wheelprobe`. On Linux the live view isn't available yet; the settings still apply.

## Steering: 900 and 270 degrees

- **900° full range** (`wheel_900_steering = 1`, the default): the capabilities report a 900 degree
  wheel, so the game uses its full-range wheel steering: the car's wheels follow your wheel's angle
  from the centre, over its whole rotation.
- **270° mode** (`wheel_900_steering = 0`): the Xbox 360 wheel's own mode, with the game's steering
  filter. `wheel_steer_offset` (0.05) then makes any movement beyond a hair already give that share of
  the full steering, against the game's inner deadzone.

`wheel_rotation` is your wheel's rotation as set in its driver, `wheel_lock` the travel that gives
full steering: 900/540 makes the steering about 1.7 times quicker.

## H-pattern shifter and clutch

With `wheel_shifter = h` and *Manual* or *Manual with clutch* shifting in the game, the lever's gear
goes to the game (reverse, neutral, 1 to 7); the game's own gearbox shifts. The Logitech Driving
Force Shifter on a G29, G920 or G923 and the shifters below are built in; bind any other in the tab.
The shifter's buttons press no other pad button. **Realistic shifting** (`wheel_shifter_clutch`)
engages a gear only with the clutch pressed past `wheel_clutch_threshold`. **Gear follows the
lever's slot** (`wheel_shifter_follow_slot`, on): race starts put the car in the lever's gear, as on
the Xbox; off, a gear engages only when the lever moves, so paddles keep their gear. The clutch
pedal is the game's analog clutch (`wheel_clutch`). The shifter type is read when the game first
sees the wheel: restart the game after changing it.

## Analog handbrake

The handbrake axis (on the wheel, or a handbrake on its own USB device) goes to the left stick Y,
which the Forza wheel layouts read as the analog handbrake next to the handbrake button. Where it
rests is read when the game opens it (keep it released then).

## Several devices

The wheel is guest user 0 (the first player); any further wheels are users 1, 2, ...; XInput pads
come after the wheels. Pedals, a shifter and a handbrake on their own devices go with the first
wheel. Unplugged while the game runs, they stop and the wheel's own pedals and buttons count again;
plugged back in, they are read again within a second.

Built in by USB id: Logitech G25, G27, G29, G920, G923 and Driving Force GT (axes, buttons,
Driving Force Shifter); Thrustmaster TMX, TX, TS-XW, TS-PC Racer, T300 RS, T150 and T-GT; pedal sets
from Simagic, Fanatec, Thrustmaster (T-LCM), Logitech, Heusinkveld and Moza; shifters (Simagic DS-8X,
Fanatec ClubSport Shifter SQ, Thrustmaster TH8 RS, Logitech's USB Driving Force Shifter, Moza HGP);
handbrakes (Simagic TB-1 and TB-RS, Fanatec ClubSport, Thrustmaster TSS). Others are found by name
("pedals", "shifter", "handbrake") or by where their axes rest: keep the pedals released while the
game starts.

## Force feedback

The game drives the 360 wheel's force feedback itself, as FH1 traced in the Forza engine. Its
XInputFF library looks up eight kernel functions when it starts (`XInputdFF*`, xboxkrnl ordinals
0x282–0x289) with `XexGetModuleHandle("xboxkrnl.exe")` and `XexGetProcedureAddress`; the kernel
answers those lookups with host functions (`src/kernel/imports_ff.cpp`). The wheel's capabilities
then claim force feedback, and the game sets up five effects and keeps them running:
- a **constant force** it updates every frame with the car's steering force;
- a **centring spring** that is off at a standstill and grows with speed;
- a light **damper**, and inertia and friction effects set to zero;
- its **rumble**, played as a light vibration (*fine*: road at 40 Hz and engine at 71 Hz) or one
  coarse 25 Hz vibration (*classic*), about 30 updates a second whatever the frame rate.

The effects play through DirectInput on every wheel with force feedback, through a second, exclusive
handle on the wheel (`src/input/wheel/wheel_system.cpp`). The wheel's own autocentre is switched off
when the game starts sending force feedback. Settings (launcher: Wheel stuff, Options, Force feedback):

- **Strength** (`wheel_ffb_strength`, 60): scales everything.
- **Centring strength** (`wheel_ffb_spring_strength`, 100): the spring and damper, in percent of the
  strength.
- **Minimum force** (`wheel_ffb_min_strength`, 0): weak steering forces are scaled up, smoothly,
  up to 4 times near the centre; in either force feedback system.
- **Rumble** (`wheel_rumble`, on): the game's rumble on the wheel, in either force feedback system,
  scaled by the strength of the one in use; **road and engine rumble** (`wheel_rumble_road_strength`
  80, `wheel_rumble_engine_strength` 50) and the **rumble style** (`wheel_rumble_style`: `fine` or
  `classic`).
- **Reverse steering force** (`wheel_ffb_reverse`): for a wheel that pulls away from the centre.

**Test force feedback** turns the wheel right, then left, through the same path at half the game's
force times the strength. If it turns left first, switch on *Reverse steering force*. It doesn't work
while the game runs, because the game holds the wheel's force feedback then. The game's log names
each call the first time (`Wheel: the game calls XInputdFFSetEffect`) and any effect the wheel
rejects.

### Comparing with FM2 ReGPU's force feedback

**Force feedback system** (`wheel_ffb_mode`) switches between this port's playback (`port`, the
default) and the one in [Forza Motorsport 2 ReGPU](https://github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU)
(`fm2`), to tell whether a difference in feel comes from the game or from the project. Both decode
the game's effects the same way (from the same FH1 patch); they differ in how they play them:

| | This port | FM2 ReGPU (`fm2`) |
| --- | --- | --- |
| Steering force | applied at each game update (about 60 a second) | eased towards each new value over ~10 ms, 500 updates a second |
| Rumble | a light vibration (fine or classic) | none of its own; this port's plays with it when **Rumble** is on |
| Fades | none | in over 0.5 s; out over 0.15 s while the window isn't in front or no steering force came for 0.25 s |
| Strength | `wheel_ffb_strength` 60, spring and damper 100 % of it | `wheel_fm2_strength` 50, spring and damper `wheel_fm2_spring` 50 % of it |
| Minimum force | `wheel_ffb_min_strength` | none of its own; this port's applies to it too (before the easing) |

FM2 also fades the forces out in menus, pauses and replays, using a hook on FM2's own code
(`hooks/wheel_driving.cpp`). FM4 has no traced counterpart yet, so the `fm2` mode leaves that out.
Reverse steering force and the test apply to both.

`wheel_ffb = 0` turns force feedback off: the capabilities don't claim it and the wheel keeps its
own autocentre. `wheel_report_ffb = 0` only hides it from the game.

## Not done

- FH1 fades the forces out while the player isn't driving (pause menu, rewind, replays). That needs
  hooks into FM4's own game state, which aren't traced yet, so the spring and damper keep their last
  values in menus.
- The game's wheel layout screens and FM4's own wheel options are untested.

## Settings (settings.ini)

| Key | Default | What it does |
| --- | --- | --- |
| `wheel_mode` | `auto` | `auto`: devices Windows lists as wheels or named like one; `on`: every joystick that isn't a gamepad; `off`: no wheels |
| `wheel_device` | found | The first player's wheel: a name, DirectInput GUID or `vid:pid` |
| `wheel_900_steering` | 1 | 1: full-range (900 degree) steering; 0: the 270 degree mode |
| `wheel_rotation`, `wheel_lock` | 900, 900 | The wheel's rotation, and the travel that gives full steering |
| `wheel_deadzone`, `wheel_invert_steering` | 0, 0 | Steering deadzone (0 to 0.5); swap left and right |
| `wheel_steer_offset` | 0.05 | 270 degree mode only: see above |
| `wheel_pedals` | `auto` | `combined`: one axis, throttle one way and brake the other |
| `wheel_invert_throttle`, `_brake`, `_clutch`, `_handbrake` | 0 | Pedals or handbrake found backwards |
| `wheel_throttle_curve`, `wheel_brake_curve` | 1 | Gamma: above 1 softer at the start |
| `wheel_axes` | detected | On the wheel: `steer=0,throttle=1,brake=2,clutch=3,handbrake=4`; 99 unbinds |
| `wheel_buttons` | by model | `a=0,b=1,x=2,y=3,lb=5,rb=4,back=8,start=9,ls=11,rs=10,up=..,down=..,left=..,right=..`; -1 unmaps; a d-pad direction set here replaces the hat's |
| `wheel_clutch` | 1 | The clutch pedal as the game's analog clutch |
| `wheel_shifter` | `off` | `h` (H-pattern) or `sequential` |
| `wheel_shifter_buttons` | by model | On the wheel: `1=12,2=13,...,6=17,r=18` or `up=..,down=..` |
| `wheel_shifter_clutch`, `wheel_clutch_threshold` | 0, 0.5 | Realistic shifting |
| `wheel_shifter_follow_slot` | 1 | The gear follows the lever's slot |
| `wheel_pedal_device`, `wheel_pedal_axes` | found | Pedals on their own device (`none`: the wheel's own); its axes `throttle=2,brake=1,clutch=0` |
| `wheel_shifter_device`, `wheel_shifter_device_buttons` | found | A shifter on its own device; its gears |
| `wheel_handbrake_device`, `wheel_handbrake_axis` | found, 0 | An analog handbrake on its own device; its axis |
| `wheel_ffb` | 1 | The game's force feedback on the wheel |
| `wheel_ffb_strength`, `wheel_ffb_spring_strength`, `wheel_ffb_min_strength` | 60, 100, 0 | Strength; the spring and damper's share; minimum steering force (percent) |
| `wheel_rumble_road_strength`, `wheel_rumble_engine_strength`, `wheel_rumble_style` | 80, 50, `fine` | The rumble motors (percent of the strength) and how they play |
| `wheel_ffb_reverse` | 0 | Reverse the steering force |
| `wheel_rumble` | 1 | The game's rumble on the wheel, with either force feedback system |
| `wheel_ffb_mode` | `port` | `fm2`: FM2 ReGPU's playback, for comparing |
| `wheel_fm2_strength`, `wheel_fm2_spring` | 50, 50 | The `fm2` playback's strength and its spring and damper share (percent) |
| `wheel_report_ffb` | 1 | 0 hides the force feedback from the game |

Axes are numbered among those a device has, in the order X, Y, Z, Rx, Ry, Rz, slider 1, slider 2;
buttons from 0. The launcher's live view shows the numbers.
