"use strict"
// The Wheel stuff tab: racing wheels, pedals, H-pattern shifters and analog handbrakes, on one USB device or several.
// The live view comes from the game's own wheel code (fm4.exe --wheel-probe, Wheel.cs), so the devices, numbers and
// what the game gets are the game's. The settings are settings.ini's wheel_* keys (src/input/wheel/wheel_config.h),
// read by the game when it starts. Uses app.js's $, el, call, toast, guarded, confirmBox.

const WH_DEFAULTS = {
  wheel_mode: "auto", wheel_900_steering: "1", wheel_rotation: "900", wheel_lock: "900", wheel_deadzone: "0",
  wheel_steer_offset: "0.05", wheel_invert_steering: "0", wheel_pedals: "auto", wheel_invert_throttle: "0",
  wheel_invert_brake: "0", wheel_invert_clutch: "0", wheel_invert_handbrake: "0", wheel_throttle_curve: "1",
  wheel_brake_curve: "1", wheel_clutch: "1", wheel_shifter: "off", wheel_shifter_clutch: "0",
  wheel_clutch_threshold: "0.5", wheel_shifter_follow_slot: "1",
  wheel_ffb: "1", wheel_ffb_strength: "60", wheel_ffb_spring_strength: "100", wheel_ffb_min_strength: "0",
  wheel_rumble_road_strength: "80", wheel_rumble_engine_strength: "50", wheel_rumble_style: "fine", wheel_ffb_reverse: "0",
  wheel_ffb_mode: "port", wheel_fm2_strength: "50", wheel_fm2_spring: "50", wheel_rumble: "1",
}
const NO_AXIS = 99 // an axis number past every device's: unbound

// Binding rows: what the game reads, by kind. id is the key in the plan and in the settings' maps.
const WH_ROWS = [
  { group: "Steering and pedals" },
  { id: "steer", label: "Steering", kind: "axis", prompt: "Turn the wheel to the right (a quarter turn)" },
  { id: "throttle", label: "Throttle", kind: "axis", prompt: "Press the throttle pedal all the way" },
  { id: "brake", label: "Brake", kind: "axis", prompt: "Press the brake pedal all the way" },
  { id: "clutch", label: "Clutch", kind: "axis", prompt: "Press the clutch pedal all the way" },
  { id: "handbrake", label: "Handbrake (analog)", kind: "axis", prompt: "Pull the handbrake all the way" },
  { group: "Buttons (read on the wheel)" },
  { id: "rb", label: "Shift up (RB, right paddle)", kind: "button" },
  { id: "lb", label: "Shift down (LB, left paddle)", kind: "button" },
  { id: "a", label: "A", kind: "button" }, { id: "b", label: "B", kind: "button" },
  { id: "x", label: "X", kind: "button" }, { id: "y", label: "Y", kind: "button" },
  { id: "back", label: "Back (view)", kind: "button" }, { id: "start", label: "Start (pause)", kind: "button" },
  { id: "ls", label: "Left stick press", kind: "button" }, { id: "rs", label: "Right stick press", kind: "button" },
  { id: "up", label: "D-pad up", kind: "button" }, { id: "down", label: "D-pad down", kind: "button" },
  { id: "left", label: "D-pad left", kind: "button" }, { id: "right", label: "D-pad right", kind: "button" },
  { group: "H-pattern shifter" },
  ...["1", "2", "3", "4", "5", "6", "7"].map((g) => ({ id: g, label: "Gear " + g, kind: "gear", prompt: `Put the lever in gear ${g}` })),
  { id: "r", label: "Reverse", kind: "gear", prompt: "Put the lever in reverse" },
  { group: "Sequential shifter" },
  { id: "up", seq: true, label: "Sequential up", kind: "gear", prompt: "Push the sequential shifter up" },
  { id: "down", seq: true, label: "Sequential down", kind: "gear", prompt: "Pull the sequential shifter down" },
]
const WH_PAD_BITS = [["up", 0x0001], ["down", 0x0002], ["left", 0x0004], ["right", 0x0008], ["start", 0x0010], ["back", 0x0020],
  ["ls", 0x0040], ["rs", 0x0080], ["lb", 0x0100], ["rb", 0x0200], ["a", 0x1000], ["b", 0x2000], ["x", 0x4000], ["y", 0x8000]]
const WH_PAD_NAMES = { up: "↑", down: "↓", left: "←", right: "→", start: "Start", back: "Back", ls: "LS", rs: "RS", lb: "LB", rb: "RB", a: "A", b: "B", x: "X", y: "Y" }

const wh = {
  shown: false,
  settings: {},     // the wheel_* keys as saved
  devices: [],      // the probe's device list
  state: null,      // the probe's last state
  selected: -1,     // the device in the live view
  bind: null,       // the row waiting for an input: { row, base, until }
  liveFor: null,    // the device the live view's elements were built for
  planKey: "",
  error: "",
  frame: 0,
}

// ---- settings

function whGet(k) { return wh.settings[k] !== undefined && wh.settings[k] !== "" ? wh.settings[k] : (WH_DEFAULTS[k] ?? "") }
function whFlag(k) { return ["1", "true", "on", "yes"].includes(String(whGet(k)).toLowerCase()) }
function whKv(key) {
  const out = {}
  for (const item of String(wh.settings[key] || "").split(",")) {
    const eq = item.indexOf("=")
    if (eq > 0) out[item.slice(0, eq).trim().toLowerCase()] = parseInt(item.slice(eq + 1), 10)
  }
  return out
}
function whSetKv(key, name, value) {
  const m = whKv(key)
  if (value === null || value === undefined) delete m[name]
  else m[name] = value
  const text = Object.entries(m).filter(([, v]) => Number.isFinite(v)).map(([k, v]) => `${k}=${v}`).join(",")
  if (text) wh.settings[key] = text
  else delete wh.settings[key]
}
let whSaveTimer = 0
function whChange(patch) {
  for (const [k, v] of Object.entries(patch || {})) {
    if (v === null || v === undefined || v === "") delete wh.settings[k]
    else wh.settings[k] = String(v)
  }
  whRenderOptions()
  whRenderBindings(true)
  clearTimeout(whSaveTimer)
  whSaveTimer = setTimeout(() => guarded(async () => { wh.settings = await call("wheel.save", wh.settings) }), 200)
}

// ---- the probe's lines (app.js routes the "wheel" events here)

function wheelEvent(d) {
  if (!d || !wh.shown) return
  if (d.t === "devices") {
    wh.devices = d.list || []
    if (wh.selected >= wh.devices.length) wh.selected = -1
    wh.liveFor = null
    whRenderDevices()
    whRenderOptions()
  } else if (d.t === "state") {
    wh.state = d
    if (wh.bind) whFeedBind(d)
    if (!wh.frame) wh.frame = requestAnimationFrame(() => { wh.frame = 0; whRenderLive() })
  } else if (d.t === "log") {
    console.log(d.line)
    // The force feedback test's outcome (wheel_system.cpp FfTest, OpenHaptic)
    if (/force feedback|rejected|vibration/.test(d.line)) $("wh-ff-status").textContent = d.line.replace(/^Wheel: /, "")
  }
}

async function wheelShow() {
  wh.shown = true
  wh.error = ""
  await guarded(async () => { wh.settings = await call("wheel.settings") })
  whRenderOptions()
  whRenderDevices()
  whRenderBindings(true)
  try { await call("wheel.start") } catch (e) { wh.error = e.message; whRenderDevices() }
}
function wheelHide() {
  if (!wh.shown) return
  wh.shown = false
  whCancelBind()
  call("wheel.stop").catch(() => {})
}

// ---- devices

const whDevName = (i) => (wh.devices[i] && wh.devices[i].name) || "?"
const whDevId = (i) => { const d = wh.devices[i]; return d ? (d.id || d.name) : "" }
const whPlan = () => (wh.state && wh.state.plan) || { wheel: -1 }
function whUseText(use) {
  if (!use) return ""
  return use.split("+").map((u) => u === "wheel" ? "the wheel" : /^wheel\d+$/.test(u) ? "wheel " + u.slice(5) + " (another controller)"
    : u === "pedals" ? "pedals, read with the wheel" : u === "shifter" ? "shifter, read with the wheel" : u === "handbrake" ? "handbrake, read with the wheel" : u).join(" · ")
}
function whRenderDevices() {
  const list = $("wh-devices")
  list.replaceChildren()
  $("wh-status").textContent = wh.error ? "" : wh.state ? "live" : "starting…"
  $("wh-status").className = "wh-status" + (wh.state && !wh.error ? " on" : "")
  if (wh.error) list.append(el("div", "empty", wh.error))
  else if (!wh.devices.length) list.append(el("div", "empty", "No wheel, pedals, shifter or handbrake found. Plug them in: the list updates by itself."))
  const use = (wh.state && wh.state.use) || []
  wh.devices.forEach((d, i) => {
    const it = el("button", "item pick" + (i === wh.selected ? " on" : ""))
    const main = el("div", "main")
    const title = el("div", "title", d.name)
    if (use[i]) title.append(el("span", "badge-on", use[i].split("+")[0].replace(/^wheel(\d+)$/, "wheel $1")))
    const bits = [d.id || "no USB id", `${d.axes.length} axes, ${d.buttons} buttons${d.hats ? ", " + d.hats + " hat" + (d.hats > 1 ? "s" : "") : ""}`]
    if (d.model) bits.push(d.model)
    if (d.ffb) bits.push("force feedback")
    main.append(title, el("div", "meta", bits.join(" · ")))
    if (use[i]) main.append(el("div", "meta wh-use", whUseText(use[i])))
    it.append(main)
    it.onclick = () => { wh.selected = i; wh.liveFor = null; whRenderDevices(); whRenderLive() }
    list.append(it)
  })
  const sel = wh.selected >= 0 ? wh.devices[wh.selected] : null
  $("wh-use-wheel").disabled = !sel || whPlan().wheel === wh.selected
  $("wh-auto-wheel").disabled = !wh.settings.wheel_device
}
$("wh-use-wheel").onclick = () => { if (wh.selected >= 0) { whChange({ wheel_device: whDevId(wh.selected) }); toast(`${whDevName(wh.selected)} is the wheel now.`, true) } }
$("wh-auto-wheel").onclick = () => whChange({ wheel_device: "" })

// ---- live view of one device, and what the game gets

function whBar(cls) { const b = el("div", "wh-bar " + (cls || "")); b.append(el("i")); return b }
function whSetBar(bar, v, centred) {
  const i = bar.firstChild
  if (centred) {
    const f = Math.max(-1, Math.min(1, v))
    i.style.left = (f < 0 ? 50 + f * 50 : 50) + "%"
    i.style.width = Math.abs(f) * 50 + "%"
  } else {
    i.style.left = "0"
    i.style.width = Math.max(0, Math.min(1, v)) * 100 + "%"
  }
}
function whBuildLive() {
  const box = $("wh-live")
  box.replaceChildren()
  const i = wh.selected
  const d = wh.devices[i]
  wh.liveFor = i
  $("wh-live-name").textContent = d ? d.name : ""
  if (!d) { box.append(el("div", "empty", "Pick a device above.")); return }
  const axes = el("div", "wh-axes")
  d.axes.forEach((name, a) => {
    const row = el("div", "wh-axis")
    row.append(el("span", "wh-axis-name", `${a} · ${name}`), whBar("centred"), el("span", "wh-axis-v", ""))
    axes.append(row)
  })
  if (!d.axes.length) axes.append(el("div", "hint", "No axes."))
  const buttons = el("div", "wh-buttons")
  for (let b = 0; b < d.buttons; b++) buttons.append(el("span", "wh-btn", String(b)))
  const hats = el("div", "wh-hats")
  for (let h = 0; h < d.hats; h++) hats.append(el("span", "wh-hat", "·"))
  box.append(axes, el("div", "wh-label", d.buttons ? "Buttons" : "No buttons"), buttons)
  if (d.hats) box.append(el("div", "wh-label", "Hats"), hats)
}
const WH_ARROWS = { 0: "·", 1: "↑", 2: "→", 3: "↗", 4: "↓", 6: "↘", 8: "←", 9: "↖", 12: "↙" }
function whRenderLive() {
  if (!wh.shown) return
  if (wh.liveFor !== wh.selected) whBuildLive()
  const s = wh.state
  const raw = s && s.raw && s.raw[wh.selected]
  if (raw) {
    const box = $("wh-live")
    box.querySelectorAll(".wh-axis").forEach((row, a) => {
      const v = raw.a[a] || 0
      whSetBar(row.children[1], v / 32768, true)
      row.children[2].textContent = v
    })
    box.querySelectorAll(".wh-btn").forEach((b, n) => b.classList.toggle("on", raw.b[n] === "1"))
    box.querySelectorAll(".wh-hat").forEach((h, n) => { h.textContent = WH_ARROWS[raw.h[n]] || "·"; h.classList.toggle("on", !!raw.h[n]) })
  }
  whRenderGame()
  whRenderBindings(false)
  // the device roles (the "use" tags) can change with the settings
  const use = s && s.use ? s.use.join("|") : ""
  if (use !== wh.useKey) {
    wh.useKey = use
    whRenderDevices()
    if (document.activeElement.tagName !== "SELECT") whRenderOptions() // the device lists leave the wheel out
  }
}
function whRenderGame() {
  const box = $("wh-game")
  const s = wh.state
  if (!box.firstChild) {
    const rows = [["steer", "Steering", true], ["throttle", "Throttle"], ["brake", "Brake"], ["clutch", "Clutch"], ["handbrake", "Handbrake"]]
    for (const [k, label, centred] of rows) {
      const r = el("div", "wh-axis")
      r.dataset.k = k
      r.append(el("span", "wh-axis-name", label), whBar(centred ? "centred" : "accent"), el("span", "wh-axis-v", ""))
      box.append(r)
    }
    const g = el("div", "wh-gearbox")
    g.append(el("div", "wh-gear", "N"), el("div", "wh-gear-sub", ""))
    box.append(g, el("div", "wh-padbtns"))
  }
  const out = (s && s.out) || {}
  const plan = whPlan()
  box.querySelectorAll(".wh-axis").forEach((r) => {
    const k = r.dataset.k, v = out[k] || 0
    whSetBar(r.children[1], v, k === "steer")
    r.children[2].textContent = k === "steer" ? Math.round(v * 100) + "%" : Math.round(v * 100) + "%"
  })
  const gearText = (g) => g === -1 ? "R" : g === 0 ? "N" : g >= 1 && g <= 8 ? String(g) : "–"
  const mode = whGet("wheel_shifter")
  box.querySelector(".wh-gear").textContent = mode === "h" ? gearText(out.gear) : "–"
  box.querySelector(".wh-gear-sub").textContent = mode === "h"
    ? `H-pattern · lever in ${gearText(out.lever)}${out.gear === 10 ? " · the paddles keep their gear" : ""}`
    : mode === "sequential" ? "Sequential shifter: up and down shift like the paddles" : "No shifter: the paddles shift"
  const pb = box.querySelector(".wh-padbtns")
  const pressed = (s && s.pad && s.pad.buttons) || 0
  if (pb.childElementCount !== WH_PAD_BITS.length) {
    pb.replaceChildren()
    for (const [k] of WH_PAD_BITS) pb.append(el("span", "wh-btn wide", WH_PAD_NAMES[k]))
  }
  WH_PAD_BITS.forEach(([, bit], n) => pb.children[n].classList.toggle("on", (pressed & bit) !== 0))
  if (plan.wheel < 0 && !box.querySelector(".hint.nowheel")) box.prepend(el("div", "hint nowheel", "No wheel yet: the game gets nothing from the devices above."))
  else if (plan.wheel >= 0) box.querySelector(".hint.nowheel")?.remove()
}

// ---- bindings

function whSourceText(src, kindText) {
  if (!src || src[0] < 0 || src[1] < 0) return null
  const d = wh.devices[src[0]]
  if (!d) return null
  const what = kindText === "axis" ? `Axis ${src[1]}${d.axes[src[1]] ? " (" + d.axes[src[1]] + ")" : ""}` : `Button ${src[1]}`
  return src[0] === whPlan().wheel ? what : `${what} on ${d.name}`
}
// Whether a row's input comes from a setting (else it is the built-in or detected one).
function whIsSet(row) {
  if (row.kind === "axis") {
    if (row.id === "handbrake") return whKv("wheel_axes").handbrake !== undefined || !!wh.settings.wheel_handbrake_device || !!wh.settings.wheel_handbrake_axis
    return whKv("wheel_axes")[row.id] !== undefined || whKv("wheel_pedal_axes")[row.id] !== undefined
  }
  if (row.kind === "button") return whKv("wheel_buttons")[row.id] !== undefined
  return whKv("wheel_shifter_buttons")[row.id] !== undefined || whKv("wheel_shifter_device_buttons")[row.id] !== undefined
}
function whRowText(row) {
  const plan = whPlan()
  if (row.kind === "axis") {
    if (row.id === "brake" && plan.combined) return { text: "Combined with the throttle (one axis)", off: true }
    const t = whSourceText(plan[row.id], "axis")
    return { text: t || "Not bound" }
  }
  if (row.kind === "button") {
    const src = plan.buttons && plan.buttons[row.id]
    const t = whSourceText(src, "button")
    if (t) return { text: t }
    const set = whKv("wheel_buttons")[row.id]
    if (set !== undefined && set >= 0) return { text: `Button ${set}: the shifter uses it`, warn: true }
    if (["up", "down", "left", "right"].includes(row.id) && set === undefined && plan.wheel >= 0 && (wh.devices[plan.wheel] || {}).hats) return { text: "Hat (d-pad)" }
    return { text: "Not bound" }
  }
  const src = plan.gears && plan.gears[row.id]
  return { text: whSourceText(src, "button") || "Not bound" }
}
function whRenderBindings(force) {
  const plan = whPlan()
  const key = JSON.stringify([plan, wh.devices.length, wh.bind && wh.bind.row.label, wh.settings])
  if (!force && key === wh.planKey) return
  wh.planKey = key
  const box = $("wh-bindings")
  box.replaceChildren()
  const mode = whGet("wheel_shifter")
  for (const row of WH_ROWS) {
    if (row.group) { box.append(el("h3", "wh-sub", row.group)); continue }
    const r = el("div", "wh-row" + (wh.bind && wh.bind.row === row ? " binding" : ""))
    const info = el("div", "main")
    const t = whRowText(row)
    info.append(el("div", "title", row.label))
    const meta = el("div", "meta" + (t.warn ? " warn" : ""), wh.bind && wh.bind.row === row ? row.prompt || "Press the button" : t.text)
    if (!t.off && !(wh.bind && wh.bind.row === row)) meta.append(el("span", "wh-def", whIsSet(row) ? "" : " · default"))
    info.append(meta)
    const bind = el("button", "ghost small", wh.bind && wh.bind.row === row ? "Cancel" : "Bind")
    bind.disabled = !!t.off || (!!wh.bind && wh.bind.row !== row) || !wh.state
    bind.onclick = () => wh.bind && wh.bind.row === row ? whCancelBind() : whStartBind(row)
    const clear = el("button", "ghost small", "Clear")
    clear.disabled = !!t.off || !!wh.bind
    clear.onclick = () => whClear(row)
    const reset = el("button", "ghost small", "Reset")
    reset.dataset.tip = "Back to the built-in or detected input."
    reset.disabled = !!wh.bind || !whIsSet(row)
    reset.onclick = () => whReset(row)
    if ((row.kind === "gear" && !row.seq && mode !== "h") || (row.seq && mode !== "sequential")) r.classList.add("dim")
    r.append(info, bind, clear, reset)
    box.append(r)
  }
  // inputs that two rows share
  const uses = new Map()
  const note = (src, label) => { if (src && src[0] >= 0 && src[1] >= 0) { const k = src.join(":"); uses.set(k, [...(uses.get(k) || []), label]) } }
  for (const row of WH_ROWS) {
    if (row.kind === "button") note(plan.buttons && plan.buttons[row.id], row.label)
    if (row.kind === "gear" && ((row.seq && mode === "sequential") || (!row.seq && mode === "h"))) note(plan.gears && plan.gears[row.id], row.label)
  }
  const shared = [...uses.entries()].filter(([, l]) => l.length > 1)
  $("wh-conflicts").hidden = !shared.length
  $("wh-conflicts").textContent = shared.length ? "Shared inputs: " + shared.map(([k, l]) => `${whSourceText(k.split(":").map(Number), "button")} is ${l.join(" and ")}`).join("; ") : ""
}

function whStartBind(row) {
  if (!wh.state) return
  wh.bind = { row, base: JSON.parse(JSON.stringify(wh.state.raw)), until: Date.now() + 10000 }
  whRenderBindings(true)
}
function whCancelBind() {
  if (!wh.bind) return
  wh.bind = null
  whRenderBindings(true)
}
addEventListener("keydown", (e) => { if (e.key === "Escape" && wh.bind) whCancelBind() })

// The first input that moves since Bind: an axis that left its resting place by half its range (steering: a fifth,
// on an axis that rests near the middle), or a button that goes down.
function whFeedBind(s) {
  const b = wh.bind
  if (Date.now() > b.until) { whCancelBind(); toast("Nothing moved: Bind again and use the control within 10 seconds."); return }
  const wantAxis = b.row.kind === "axis", wantButton = !wantAxis
  for (let i = 0; i < s.raw.length; i++) {
    const now = s.raw[i], base = b.base[i]
    if (!base || base.a.length !== now.a.length) continue
    if (wantAxis) {
      const steer = b.row.id === "steer"
      for (let a = 0; a < now.a.length; a++) {
        const delta = now.a[a] - base.a[a]
        if (steer && Math.abs(base.a[a]) > 12000) continue
        if (Math.abs(delta) >= (steer ? 5000 : 16000)) return whApplyAxis(b.row, i, a, Math.sign(delta), base.a[a])
      }
    }
    if (wantButton) {
      let bb = ""
      for (let n = 0; n < now.b.length; n++) {
        if (now.b[n] === "1" && base.b[n] === "0") return whApplyButton(b.row, i, n)
        bb += now.b[n] === "1" && base.b[n] === "1" ? "1" : "0" // a button held at Bind counts once let go
      }
      base.b = bb
    }
  }
}

function whApplyAxis(row, dev, axis, dir, rest) {
  wh.bind = null
  const plan = whPlan()
  const d = wh.devices[dev]
  const patch = {}
  if (row.id === "steer") {
    // The device you turn is the wheel.
    if (plan.wheel !== dev) { wh.settings.wheel_device = whDevId(dev) }
    whSetKv("wheel_axes", "steer", axis)
    wh.settings.wheel_invert_steering = dir < 0 ? "1" : "0"
    whChange(patch)
    return toast(`Steering: axis ${axis} (${d.axes[axis]}) on ${d.name}${dir < 0 ? ", inverted" : ""}.`, true)
  }
  if (plan.wheel < 0) { whChange(); return toast("Bind the steering first: the device you turn becomes the wheel.") }
  // A pedal or handbrake rests at one end; the game takes pressing towards the other end as pressed, from where it rested
  // when opened (a known Logitech wheel's pedals: from the maximum). Pressed the other way, its invert switch goes on.
  const known = dev === plan.wheel && d.profile && row.id !== "handbrake"
  const highRest = known ? true : (d.rest && d.rest[axis] !== undefined ? d.rest[axis] : rest) > 0
  const invert = row.kind === "axis" && (highRest ? dir > 0 : dir < 0)
  if (row.id === "handbrake") {
    if (dev === plan.wheel) {
      whSetKv("wheel_axes", "handbrake", axis)
    } else {
      whSetKv("wheel_axes", "handbrake", null)
      wh.settings.wheel_handbrake_device = whDevId(dev)
      wh.settings.wheel_handbrake_axis = String(axis)
    }
    wh.settings.wheel_invert_handbrake = invert ? "1" : "0"
  } else {
    const combined = whGet("wheel_pedals") === "combined"
    if (dev === plan.wheel) {
      whSetKv("wheel_axes", row.id, axis)
      whSetKv("wheel_pedal_axes", row.id, null)
    } else {
      wh.settings.wheel_pedal_device = whDevId(dev)
      whSetKv("wheel_pedal_axes", row.id, axis)
      whSetKv("wheel_axes", row.id, null)
    }
    // Combined pedals: the throttle is the axis's negative side.
    wh.settings["wheel_invert_" + row.id] = (combined && row.id === "throttle" ? dir > 0 : invert) ? "1" : "0"
  }
  whChange(patch)
  toast(`${row.label}: axis ${axis} (${d.axes[axis]}) on ${d.name}${wh.settings["wheel_invert_" + (row.id === "handbrake" ? "handbrake" : row.id)] === "1" ? ", inverted" : ""}.`, true)
}

function whApplyButton(row, dev, n) {
  wh.bind = null
  const plan = whPlan()
  const d = wh.devices[dev]
  if (plan.wheel < 0) { whChange(); return toast("Bind the steering first: the device you turn becomes the wheel.") }
  if (row.kind === "button") {
    if (dev !== plan.wheel) { whChange(); return toast(`Buttons are read on the wheel (${whDevName(plan.wheel)}): press one there, or make ${d.name} the wheel.`) }
    whSetKv("wheel_buttons", row.id, n)
    whChange()
    return toast(`${row.label}: button ${n}.`, true)
  }
  // a gear: on the wheel, or on its own device (which becomes the shifter device)
  const patch = {}
  if (dev === plan.wheel) {
    whSetKv("wheel_shifter_buttons", row.id, n)
    if (plan.shifter >= 0) patch.wheel_shifter_device = "none" // read the gears from the wheel, not the shifter device
  } else {
    // a new shifter device: its gears start from the known model's (or nothing)
    if (plan.shifter >= 0 && plan.shifter !== dev) delete wh.settings.wheel_shifter_device_buttons
    patch.wheel_shifter_device = whDevId(dev) // kept by its USB id, also when it was found by itself
    whSetKv("wheel_shifter_device_buttons", row.id, n)
  }
  const mode = whGet("wheel_shifter")
  if (row.seq && mode !== "sequential") patch.wheel_shifter = "sequential"
  if (!row.seq && mode !== "h") patch.wheel_shifter = "h"
  whChange(patch)
  toast(`${row.label}: button ${n} on ${d.name}${patch.wheel_shifter ? `; shifter set to ${patch.wheel_shifter === "h" ? "H-pattern" : "sequential"}` : ""}.`, true)
}

function whClear(row) {
  const plan = whPlan()
  if (row.kind === "axis") {
    if (row.id === "handbrake") whSetKv("wheel_axes", "handbrake", NO_AXIS) // also no handbrake device
    else if (row.id !== "steer" && plan[row.id] && plan[row.id][0] >= 0 && plan[row.id][0] !== plan.wheel) whSetKv("wheel_pedal_axes", row.id, NO_AXIS)
    else whSetKv("wheel_axes", row.id, NO_AXIS)
  } else if (row.kind === "button") whSetKv("wheel_buttons", row.id, -1)
  else whSetKv(plan.shifter >= 0 && whGet("wheel_shifter_device") !== "none" ? "wheel_shifter_device_buttons" : "wheel_shifter_buttons", row.id, -1)
  whChange()
}
function whReset(row) {
  const patch = {}
  if (row.kind === "axis") {
    whSetKv("wheel_axes", row.id, null)
    whSetKv("wheel_pedal_axes", row.id, null)
    if (row.id === "handbrake") Object.assign(patch, { wheel_handbrake_device: "", wheel_handbrake_axis: "", wheel_invert_handbrake: "" })
    if (row.id === "steer") patch.wheel_invert_steering = ""
    if (["throttle", "brake", "clutch"].includes(row.id)) patch["wheel_invert_" + row.id] = ""
  } else if (row.kind === "button") whSetKv("wheel_buttons", row.id, null)
  else {
    whSetKv("wheel_shifter_buttons", row.id, null)
    whSetKv("wheel_shifter_device_buttons", row.id, null)
  }
  whChange(patch)
}
$("wh-reset-all").onclick = () => guarded(async () => {
  if (!await confirmBox("Reset all wheel bindings?", "Every binding and device choice on this page goes back to the built-in and detected ones. The options (steering mode, shifter, curves, ...) stay.", "Reset")) return
  const patch = {}
  for (const k of ["wheel_axes", "wheel_buttons", "wheel_pedal_axes", "wheel_shifter_buttons", "wheel_shifter_device_buttons", "wheel_device",
    "wheel_pedal_device", "wheel_shifter_device", "wheel_handbrake_device", "wheel_handbrake_axis", "wheel_invert_steering",
    "wheel_invert_throttle", "wheel_invert_brake", "wheel_invert_clutch", "wheel_invert_handbrake"]) patch[k] = ""
  whChange(patch)
})

// ---- options

function whSeg(id, key) {
  for (const b of $(id).children) b.classList.toggle("on", b.dataset.v === (key === "wheel_pedals" && whGet(key) === "separate" ? "auto" : String(whGet(key))))
}
function whDeviceOptions(select, key) {
  const v = wh.settings[key] || ""
  const opts = [{ value: "", name: "Auto (find one)" }, { value: "none", name: "None (the wheel's own)" }]
  const plan = whPlan()
  wh.devices.forEach((d, i) => { if (i !== plan.wheel || whDevId(i) === v) opts.push({ value: whDevId(i), name: d.name + (d.id ? ` (${d.id})` : "") }) })
  if (v && !opts.some((o) => o.value.toLowerCase() === v.toLowerCase())) opts.push({ value: v, name: `${v} (not connected)` })
  select.replaceChildren()
  for (const o of opts) { const e = el("option", "", o.name); e.value = o.value; select.append(e) }
  select.value = opts.find((o) => o.value.toLowerCase() === v.toLowerCase())?.value ?? ""
}
function whRenderOptions() {
  for (const b of $("wh-steer-mode").children) b.classList.toggle("on", (b.dataset.v === "1") === whFlag("wheel_900_steering"))
  whSeg("wh-pedals", "wheel_pedals")
  whSeg("wh-shifter", "wheel_shifter")
  for (const sw of document.querySelectorAll("#page-wheel [data-flag]")) sw.classList.toggle("on", whFlag(sw.dataset.flag))
  const num = (id, key) => { if (document.activeElement !== $(id)) $(id).value = whGet(key) }
  num("wh-rotation", "wheel_rotation")
  num("wh-lock", "wheel_lock")
  const pct = (id, key, scale) => { const v = parseFloat(whGet(key)) || 0; $(id).value = Math.round(v * scale); $(id + "-v").textContent = scale === 100 && id !== "wh-tcurve" && id !== "wh-bcurve" ? Math.round(v * 100) + "%" : v.toFixed(2) }
  pct("wh-deadzone", "wheel_deadzone", 100)
  pct("wh-offset", "wheel_steer_offset", 100)
  pct("wh-thr", "wheel_clutch_threshold", 100)
  pct("wh-tcurve", "wheel_throttle_curve", 100)
  pct("wh-bcurve", "wheel_brake_curve", 100)
  $("wh-offset-field").classList.toggle("off", whFlag("wheel_900_steering"))
  for (const r of document.querySelectorAll("#page-wheel [data-int]")) {
    const v = parseInt(whGet(r.dataset.int), 10) || 0
    if (document.activeElement !== r) r.value = v
    $(r.id + "-v").textContent = v + "%"
  }
  whSeg("wh-rumble", "wheel_rumble_style")
  whSeg("wh-ffmode", "wheel_ffb_mode")
  const fm2 = whGet("wheel_ffb_mode") === "fm2"
  $("wh-fm2-opts").hidden = !fm2
  $("wh-port-opts").hidden = fm2
  $("wh-rumble-opts").classList.toggle("off", !whFlag("wheel_rumble"))
  fill($("wh-mode"), [{ value: "auto", name: "Auto" }, { value: "on", name: "On: every joystick that isn't a gamepad" }, { value: "off", name: "Off" }], whGet("wheel_mode"))
  whDeviceOptions($("wh-pedal-device"), "wheel_pedal_device")
  whDeviceOptions($("wh-shifter-device"), "wheel_shifter_device")
  whDeviceOptions($("wh-handbrake-device"), "wheel_handbrake_device")
}
$("wh-steer-mode").onclick = (e) => { const b = e.target.closest("button"); if (b) whChange({ wheel_900_steering: b.dataset.v }) }
$("wh-pedals").onclick = (e) => { const b = e.target.closest("button"); if (b) whChange({ wheel_pedals: b.dataset.v === "auto" ? "" : b.dataset.v }) }
$("wh-shifter").onclick = (e) => { const b = e.target.closest("button"); if (b) whChange({ wheel_shifter: b.dataset.v }) }
for (const sw of document.querySelectorAll("#page-wheel [data-flag]")) sw.onclick = () => whChange({ [sw.dataset.flag]: whFlag(sw.dataset.flag) ? "0" : "1" })
$("wh-rotation").onchange = (e) => { const v = Math.min(2520, Math.max(90, Math.round(+e.target.value) || 900)); whChange({ wheel_rotation: v }) }
$("wh-lock").onchange = (e) => { const v = Math.min(2520, Math.max(90, Math.round(+e.target.value) || 900)); whChange({ wheel_lock: v }) }
for (const [id, key] of [["wh-deadzone", "wheel_deadzone"], ["wh-offset", "wheel_steer_offset"], ["wh-thr", "wheel_clutch_threshold"], ["wh-tcurve", "wheel_throttle_curve"], ["wh-bcurve", "wheel_brake_curve"]]) {
  $(id).oninput = (e) => { $(id + "-v").textContent = (key.endsWith("curve") ? (+e.target.value / 100).toFixed(2) : e.target.value + "%") }
  $(id).onchange = (e) => whChange({ [key]: (+e.target.value / 100).toString() })
}
for (const r of document.querySelectorAll("#page-wheel [data-int]")) {
  r.oninput = () => { $(r.id + "-v").textContent = r.value + "%" }
  r.onchange = () => whChange({ [r.dataset.int]: String(Math.round(+r.value)) })
}
$("wh-ffmode").onclick = (e) => { const b = e.target.closest("button"); if (b) whChange({ wheel_ffb_mode: b.dataset.v === "port" ? "" : b.dataset.v }) }
$("wh-rumble").onclick = (e) => { const b = e.target.closest("button"); if (b) whChange({ wheel_rumble_style: b.dataset.v }) }
$("wh-fftest").onclick = () => guarded(async () => {
  $("wh-ff-status").textContent = "Testing: the wheel turns right, then left…"
  await call("wheel.fftest")
})
$("wh-mode").onchange = (e) => whChange({ wheel_mode: e.target.value === "auto" ? "" : e.target.value })
$("wh-pedal-device").onchange = (e) => whChange({ wheel_pedal_device: e.target.value })
$("wh-shifter-device").onchange = (e) => whChange({ wheel_shifter_device: e.target.value })
$("wh-handbrake-device").onchange = (e) => whChange({ wheel_handbrake_device: e.target.value })
