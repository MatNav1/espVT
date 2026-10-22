// Robust parametric model/export using Manifold 3D.
// Source dimensions match esp32-s3-touch-lcd-7b-case.js.

import fs from 'node:fs'
import Module from 'manifold-3d'

const wasm = await Module()
wasm.setup()
const { Manifold, CrossSection } = wasm

const P = {
  glass: [192.96, 110.76], glassClearance: 0.30,
  screenOpening: [190.0, 107.8], outer: [212.0, 130.0],
  frontThickness: 5.0, frontLip: 2.0,
  rearDepth: 30.0, rearBack: 4.0, wall: 3.0,
  um982RearDepth: 48.0, rs232RearDepth: 40.0,
  caseScrewXY: [101.0, 60.0], caseClearanceDiameter: 3.4,
  caseInsertHoleDiameter: 4.1,
  mountClearanceDiameter: 4.5, nutAcrossFlats: 7.3, nutDepth: 3.5,
  vesaSpacing: [75.0, 75.0], ampsSpacing: [38.0, 30.0],
  switchCutout: [19.4, 13.4], switchX: 30.0,
  cableHoleDiameter: 12.5, cableX: -55.0,
  um982Board: [25.66, 36.58, 6.1], um982HoleSpacing: [20.07, 30.86],
  um982Center: [-72.0, 35.0], um982PilotDiameter: 2.1,
  rs232UniversalArea: [82.0, 56.0],
  smaBulkheadDiameter: 6.8,
  um982UsbOpening: [12.0, 8.0],
  db9Opening: [20.5, 11.0], db9ScrewSpacing: 25.0,
  modularPanelCenterXZ: [72.0, 24.0], modularPanelOuter: [56.0, 46.0],
  modularPanelOpening: [44.0, 34.0], modularPanelScrewSpacing: [48.0, 40.0],
  modularUm982Center: [-75.0, 20.0],
  displayMountSpacing: [126.20, 65.65],
  rearInsertFrontDepth: 13.0,
  screenFaceRecess: 1.8,
  screenFrontToMountPlane: 5.0,
  screenPocketClearance: 0.35
}

function roundedBox (size, center, radius = 2, segments = 32) {
  const cs = CrossSection.square([size[0] - 2 * radius, size[1] - 2 * radius], true)
    .offset(radius, 'Round', 2, segments)
  return cs.extrude(size[2]).translate([center[0], center[1], center[2] - size[2] / 2])
}

function zCylinder (diameter, height, centerZ, xy = [0, 0], segments = 48) {
  return Manifold.cylinder(height, diameter / 2, diameter / 2, segments, true)
    .translate([xy[0], xy[1], centerZ])
}

function yCylinder (diameter, height, centerY, xz = [0, 0], segments = 48) {
  return Manifold.cylinder(height, diameter / 2, diameter / 2, segments, true)
    .rotate([90, 0, 0]).translate([xz[0], centerY, xz[1]])
}

function xzRoundedBox (size, center, radius = 2, segments = 32) {
  return roundedBox([size[0], size[1], size[2]], [0, 0, 0], radius, segments)
    .rotate([90, 0, 0]).translate([center[0], center[1], center[2]])
}

function pattern (spacing) {
  const x = spacing[0] / 2, y = spacing[1] / 2
  return [[-x, -y], [-x, y], [x, -y], [x, y]]
}

function unionAll (items) { return Manifold.union(items) }
function cutAll (base, cuts) { return Manifold.difference([base, ...cuts]) }

function frontBezel () {
  const pocket = [P.glass[0] + 2 * P.glassClearance, P.glass[1] + 2 * P.glassClearance]
  let body = roundedBox([P.outer[0], P.outer[1], P.frontThickness], [0, 0, P.frontThickness / 2], 8)
  body = cutAll(body, [
    roundedBox([P.screenOpening[0], P.screenOpening[1], P.frontThickness + 2], [0, 0, P.frontThickness / 2], 4),
    roundedBox([pocket[0], pocket[1], P.frontThickness], [0, 0, P.frontLip + P.frontThickness / 2], 5)
  ])

  const [sx, sy] = P.caseScrewXY
  const points = [[-sx, -sy], [-sx, sy], [sx, -sy], [sx, sy]]
  const bosses = points.map(p => zCylinder(8.5, 7.5, P.frontThickness + 3.25, p))
  body = unionAll([body, ...bosses])
  const insertHoles = points.map(p => zCylinder(P.caseInsertHoleDiameter, 6.6, P.frontThickness + 4.0, p))
  return cutAll(body, insertHoles)
}

function rearInsertFrontHousing () {
  const depth = P.rearInsertFrontDepth
  const pocket = [
    P.glass[0] + 2 * P.screenPocketClearance,
    P.glass[1] + 2 * P.screenPocketClearance
  ]

  // A narrow protective surround, open at the front and rear. The display
  // slides in from the rear; its face sits below the front edge by 1.8 mm.
  let body = cutAll(
    roundedBox([P.outer[0], P.outer[1], depth], [0, 0, depth / 2], 8),
    [roundedBox([pocket[0], pocket[1], depth + 2], [0, 0, depth / 2], 8.2, 40)]
  )

  const displayPoints = pattern(P.displayMountSpacing)
  const padFront = P.screenFaceRecess + P.screenFrontToMountPlane
  const padHeight = 3.2
  const pads = displayPoints.map(p =>
    zCylinder(11.0, padHeight, padFront + padHeight / 2, p, 48))

  // Each mounting pad is tied to the nearest long wall. These bridges sit
  // behind the LCD and do not form a visible bezel around the glass.
  const halfPocketY = pocket[1] / 2
  const bridges = displayPoints.map(p => {
    const edgeY = Math.sign(p[1]) * halfPocketY
    const bridgeCenterY = (p[1] + edgeY) / 2
    const bridgeLength = Math.abs(edgeY - p[1]) + 2
    return roundedBox([9.0, bridgeLength, padHeight + 0.4],
      [p[0], bridgeCenterY, padFront + padHeight / 2 - 0.1], 2.0, 20)
  })

  // Existing long through-screws in the rear cover finish in heat-set inserts
  // installed from the rear of this front housing.
  const [sx, sy] = P.caseScrewXY
  const casePoints = [[-sx, -sy], [-sx, sy], [sx, -sy], [sx, sy]]
  const caseBosses = casePoints.map(p =>
    zCylinder(8.5, depth - 4.0, (depth + 4.0) / 2, p))

  // A short locating tongue fits the existing 206 x 124 mm rear-shell cavity.
  const tongueOuter = [P.outer[0] - 2 * P.wall - 0.6, P.outer[1] - 2 * P.wall - 0.6]
  const tongue = cutAll(
    roundedBox([tongueOuter[0], tongueOuter[1], 1.5], [0, 0, depth + 0.75], 5.0, 28),
    [roundedBox([pocket[0], pocket[1], 2.5], [0, 0, depth + 0.75], 8.2, 40)]
  )

  body = unionAll([body, tongue, ...pads, ...bridges, ...caseBosses])

  const cuts = []
  // Clearance holes: M3 screws pass from the rear through the printed pads
  // directly into the four factory threaded metal tabs.
  for (const p of displayPoints) {
    cuts.push(zCylinder(3.4, padHeight + 1.2, padFront + padHeight / 2, p, 36))
  }
  for (const p of casePoints) {
    cuts.push(zCylinder(P.caseInsertHoleDiameter, 6.2, depth - 3.1, p, 40))
  }
  return cutAll(body, cuts)
}

function rearShell (depth = P.rearDepth, variant = 'standard') {
  const withUm982 = variant === 'um982' || variant === 'combined'
  const withRs232 = variant === 'rs232' || variant === 'combined'
  const [ow, oh] = P.outer
  let body = cutAll(
    roundedBox([ow, oh, depth], [0, 0, depth / 2], 8),
    [roundedBox([ow - 2 * P.wall, oh - 2 * P.wall, depth - P.rearBack + 2],
      [0, 0, P.rearBack + (depth - P.rearBack + 2) / 2], 5)]
  )

  const [sx, sy] = P.caseScrewXY
  const casePoints = [[-sx, -sy], [-sx, sy], [sx, -sy], [sx, sy]]
  const guideH = depth - P.rearBack + 0.5
  const guides = casePoints.map(p => zCylinder(8.5, guideH, P.rearBack - 0.5 + guideH / 2, p))

  const mountPoints = [...pattern(P.vesaSpacing), ...pattern(P.ampsSpacing)]
  const mountBosses = mountPoints.map(p => zCylinder(12, 5.5, P.rearBack + 2.25, p))
  body = unionAll([body, ...guides, ...mountBosses])

  let um982Points = []
  if (withUm982) {
    // Dedicated M2.5 self-tapping standoffs for the user's 25.66 x 36.58 mm UM982 carrier.
    um982Points = pattern(P.um982HoleSpacing).map(p => [p[0] + P.um982Center[0], p[1] + P.um982Center[1]])
    const um982Bosses = um982Points.map(p => zCylinder(7.0, 5.5, P.rearBack + 2.25, p))
    body = unionAll([body, ...um982Bosses])
  }

  if (withRs232) {
    // Raised universal rails for a compact TTL-to-RS232 converter. The open
    // centre leaves the VESA/AMPS nut traps accessible and accepts zip ties.
    const railX = 28.0
    const railY = P.rs232UniversalArea[1] / 2
    const rails = [
      roundedBox([P.rs232UniversalArea[0], 6.0, 2.5], [railX, -railY, 11.0], 2.5, 24),
      roundedBox([P.rs232UniversalArea[0], 6.0, 2.5], [railX, railY, 11.0], 2.5, 24)
    ]
    const railSupports = [
      [railX - 37, -railY], [railX + 37, -railY],
      [railX - 37, railY], [railX + 37, railY]
    ].map(p => zCylinder(8.0, 7.5, 7.25, p))
    body = unionAll([body, ...rails, ...railSupports])
  }

  const cuts = []
  for (const p of casePoints) {
    cuts.push(zCylinder(P.caseClearanceDiameter, depth + 2, depth / 2, p))
    cuts.push(zCylinder(6.5, 3.0, 1.5, p))
  }

  const hexRadius = P.nutAcrossFlats / Math.sqrt(3)
  for (const p of mountPoints) {
    cuts.push(zCylinder(P.mountClearanceDiameter, P.rearBack + 8, (P.rearBack + 4) / 2, p))
    cuts.push(zCylinder(2 * hexRadius, P.nutDepth + 0.4,
      P.rearBack + 5.0 - (P.nutDepth + 0.4) / 2, p, 6))
  }

  for (const y of [-48, 48]) for (const x of [-60, -30, 0, 30, 60]) {
    cuts.push(roundedBox([22, 3.2, P.rearBack + 2], [x, y, P.rearBack / 2], 1.5, 16))
  }

  cuts.push(roundedBox([P.switchCutout[0], P.wall + 4, P.switchCutout[1]],
    [P.switchX, -oh / 2, P.rearBack + 12], 1.0, 16))

  cuts.push(Manifold.cylinder(P.wall + 5, P.cableHoleDiameter / 2,
    P.cableHoleDiameter / 2, 48, true).rotate([90, 0, 0])
    .translate([P.cableX, -oh / 2, P.rearBack + 12]))

  // Slots for zip ties securing a compact DC/DC converter, approx. 42 x 24 mm.
  cuts.push(roundedBox([3.2, 18, P.rearBack + 2], [48, 35, P.rearBack / 2], 1.5, 16))
  cuts.push(roundedBox([3.2, 18, P.rearBack + 2], [78, 35, P.rearBack / 2], 1.5, 16))

  if (withUm982) {
    // Blind pilot holes in the UM982 standoffs.
    for (const p of um982Points) cuts.push(zCylinder(P.um982PilotDiameter, 4.2, 7.9, p, 32))

    // Two M6/SMA bulkhead passages aligned above the UM982 antenna connectors.
    for (const x of [P.um982Center[0] - 10, P.um982Center[0] + 10]) {
      cuts.push(Manifold.cylinder(P.wall + 5, P.smaBulkheadDiameter / 2,
        P.smaBulkheadDiameter / 2, 48, true).rotate([90, 0, 0])
        .translate([x, oh / 2, 20]))
    }

    // USB-C service opening aligned with the top edge of the UM982 carrier.
    cuts.push(roundedBox([P.um982UsbOpening[0], P.wall + 5, P.um982UsbOpening[1]],
      [P.um982Center[0], oh / 2, 11.8], 1.5, 20))

  }

  if (withRs232) {
    // Panel-mount DE-9/DB9 opening and two M3 flange holes in the bottom wall.
    const db9X = 70.0, db9Z = 20.0
    cuts.push(roundedBox([P.db9Opening[0], P.wall + 5, P.db9Opening[1]],
      [db9X, -oh / 2, db9Z], 2.0, 24))
    for (const x of [-P.db9ScrewSpacing / 2, P.db9ScrewSpacing / 2]) {
      cuts.push(Manifold.cylinder(P.wall + 5, 1.65, 1.65, 32, true)
        .rotate([90, 0, 0]).translate([db9X + x, -oh / 2, db9Z]))
    }
  }

  return cutAll(body, cuts)
}

function modularPanelScrewPoints () {
  const c = P.modularPanelCenterXZ
  return pattern(P.modularPanelScrewSpacing).map(p => [p[0] + c[0], p[1] + c[1]])
}

function modularRearShell () {
  let body = rearShell(P.um982RearDepth, 'standard')
  const c = P.modularPanelCenterXZ
  const screwPoints = modularPanelScrewPoints()

  // The UM982 carrier is fixed to the inside of the rear wall. Its antenna
  // sockets are connected by short coax pigtails to the interchangeable panel.
  const umCenter = P.modularUm982Center
  const um982Points = pattern(P.um982HoleSpacing)
    .map(p => [p[0] + umCenter[0], p[1] + umCenter[1]])
  const um982Bosses = um982Points.map(p => zCylinder(7.0, 5.5, P.rearBack + 2.25, p))

  // Reinforced M3 bosses run inward from the lower wall (Y axis).
  const bosses = screwPoints.map(p => yCylinder(8.0, 8.5, -58.25, p))
  body = unionAll([body, ...bosses, ...um982Bosses])

  const cuts = [
    xzRoundedBox([P.modularPanelOpening[0], P.modularPanelOpening[1], P.wall + 5],
      [c[0], -P.outer[1] / 2, c[1]], 3.0, 28)
  ]
  // Heat-set inserts are installed from the underside before fitting a panel.
  for (const p of screwPoints) cuts.push(yCylinder(4.1, 7.0, -62.5, p, 40))
  // Blind pilot holes for four short M2.5 screws holding the UM982 carrier.
  for (const p of um982Points) cuts.push(zCylinder(P.um982PilotDiameter, 4.2, 7.9, p, 32))
  return cutAll(body, cuts)
}

function panelBase () {
  const outer = roundedBox([P.modularPanelOuter[0], P.modularPanelOuter[1], 3.0], [0, 0, 1.5], 5.0, 32)
  // Locating tongue: 0.3 mm total clearance relative to the rear-shell opening.
  const tongue = roundedBox([P.modularPanelOpening[0] - 0.3, P.modularPanelOpening[1] - 0.3, 1.5],
    [0, 0, 3.75], 3.7, 28)
  let panel = unionAll([outer, tongue])
  const holes = pattern(P.modularPanelScrewSpacing).map(p => zCylinder(3.4, 7, 2.5, p, 36))
  return cutAll(panel, holes)
}

function um982Panel () {
  const panel = panelBase()
  // Only the two antenna bulkheads remain in this panel. The UM982 itself is
  // mounted on the rear wall and connected with short coax pigtails.
  const cuts = [-10, 10].map(x => zCylinder(P.smaBulkheadDiameter, 7, 2.5, [x, 0], 48))
  return cutAll(panel, cuts)
}

function rs232Panel () {
  let panel = panelBase()
  // Raised internal rails for a compact TTL-to-RS232 converter secured by zip ties.
  const rails = [
    roundedBox([36, 5, 2.5], [0, -14, 5.25], 2.0, 20),
    roundedBox([36, 5, 2.5], [0, -3, 5.25], 2.0, 20)
  ]
  panel = unionAll([panel, ...rails])

  const db9Y = 12
  const cuts = [roundedBox([P.db9Opening[0], P.db9Opening[1], 7], [0, db9Y, 2.5], 2.0, 24)]
  for (const x of [-P.db9ScrewSpacing / 2, P.db9ScrewSpacing / 2]) {
    cuts.push(zCylinder(3.3, 7, 2.5, [x, db9Y], 32))
  }
  return cutAll(panel, cuts)
}

function switchTestCoupon () {
  const plate = roundedBox([40, 30, 3], [0, 0, 1.5], 3, 24)
  const opening = roundedBox([P.switchCutout[0], P.switchCutout[1], 5], [0, 0, 1.5], 1.0, 16)
  return plate.subtract(opening)
}

function db9TestCoupon () {
  let plate = roundedBox([45, 25, 3], [0, 0, 1.5], 3, 24)
  const cuts = [roundedBox([P.db9Opening[0], P.db9Opening[1], 5], [0, 0, 1.5], 2.0, 24)]
  for (const x of [-P.db9ScrewSpacing / 2, P.db9ScrewSpacing / 2]) {
    cuts.push(zCylinder(3.3, 5, 1.5, [x, 0], 32))
  }
  return cutAll(plate, cuts)
}

function displayMountTestGauge () {
  const [sx, sy] = P.displayMountSpacing
  const points = pattern(P.displayMountSpacing)
  const rails = [
    roundedBox([sx + 10, 5, 3], [0, -sy / 2, 1.5], 2.0, 20),
    roundedBox([sx + 10, 5, 3], [0, sy / 2, 1.5], 2.0, 20),
    roundedBox([5, sy + 10, 3], [-sx / 2, 0, 1.5], 2.0, 20),
    roundedBox([5, sy + 10, 3], [sx / 2, 0, 1.5], 2.0, 20)
  ]
  const pads = points.map(p => zCylinder(10, 3.4, 1.7, p, 40))
  const holes = points.map(p => zCylinder(3.4, 5, 1.7, p, 36))
  return cutAll(unionAll([...rails, ...pads]), holes)
}

function writeBinaryStl (path, solid) {
  const mesh = solid.getMesh()
  const tri = mesh.triVerts
  const verts = mesh.vertProperties
  const n = tri.length / 3
  const out = Buffer.alloc(84 + n * 50)
  out.write('Manifold3D ESP32-S3 Touch LCD 7B enclosure', 0, 'ascii')
  out.writeUInt32LE(n, 80)
  let off = 84
  for (let i = 0; i < n; i++) {
    const ids = [tri[3 * i], tri[3 * i + 1], tri[3 * i + 2]]
    const pts = ids.map(id => [verts[mesh.numProp * id], verts[mesh.numProp * id + 1], verts[mesh.numProp * id + 2]])
    const a = pts[0], b = pts[1], c = pts[2]
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]
    const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
    let normal = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    const len = Math.hypot(...normal) || 1
    normal = normal.map(x => x / len)
    for (const x of normal) { out.writeFloatLE(x, off); off += 4 }
    for (const p of pts) for (const x of p) { out.writeFloatLE(x, off); off += 4 }
    out.writeUInt16LE(0, off); off += 2
  }
  fs.writeFileSync(path, out)
  console.log(`${path}: triangles=${n}, volume=${solid.volume().toFixed(1)} mm^3, status=${solid.status()}`)
}

writeBinaryStl('ESP32-S3-Touch-LCD-7B_front_PETG.stl', frontBezel())
writeBinaryStl('ESP32-S3-Touch-LCD-7B_front_REAR_INSERT_PETG.stl', rearInsertFrontHousing())
writeBinaryStl('ESP32-S3-Touch-LCD-7B_rear_VESA75_AMPS_PETG.stl', rearShell())
writeBinaryStl('ESP32-S3-Touch-LCD-7B_rear_UM982_PETG.stl', rearShell(P.um982RearDepth, 'um982'))
writeBinaryStl('ESP32-S3-Touch-LCD-7B_rear_RS232_PETG.stl', rearShell(P.rs232RearDepth, 'rs232'))
writeBinaryStl('ESP32-S3-Touch-LCD-7B_rear_MODULAR_PETG.stl', modularRearShell())
writeBinaryStl('ESP32-S3-Touch-LCD-7B_panel_UM982_PETG.stl', um982Panel())
writeBinaryStl('ESP32-S3-Touch-LCD-7B_panel_RS232_PETG.stl', rs232Panel())
writeBinaryStl('test_switch_19.4x13.4_PETG.stl', switchTestCoupon())
writeBinaryStl('test_DB9_panel_PETG.stl', db9TestCoupon())
writeBinaryStl('test_Waveshare_M3_126.20x65.65_PETG.stl', displayMountTestGauge())

fs.writeFileSync('case-parameters.json', JSON.stringify(P, null, 2) + '\n')
