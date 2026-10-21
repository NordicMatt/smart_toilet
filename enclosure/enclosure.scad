/*
  Parametric enclosure for nRF54LM20DK project box.

  Default view renders base and lid separated for printing/inspection.
  Use:
    base();
    lid();
    assembled();
    mic_section();

  The PDM mic breakout mounts in a recess in the lid's underside, mic facing
  up into a shallow pocket under a 1.2 mm skin; heat-stake its two pegs over
  the board (mount from the ml_demo/enclosure mic test). The lid prints
  letters-up with no supports.
*/

$fn = 64;

use <magic_symbols.scad>   // Symbols traced from the Freepik abstract-symbol pack.

// ---------------------------------------------------------------------------
// Key dimensions
// ---------------------------------------------------------------------------

pcb_length = 145;
pcb_width = 63.5;
pcb_height = 10;

pcb_clearance  = 3;         // Internal clearance at each end (short walls).
side_clearance = 10;        // Internal clearance each long side — extra room for corner boss columns.
standoff_height = 6;        // PCB bottom height above enclosure floor — 6 mm gives M3 thread engagement.
height_above_pcb = 62;      // Free internal height above PCB top for 50.8 mm nRF7002 EB + ~8.5 mm header + margin.

wall_thickness = 2.5;
floor_thickness = 2.5;
lid_thickness_min = 3.5;    // Room for blind locating-pin pockets; the mic pocket may need more (see derived lid_thickness).
corner_radius = 3;

standoff_radius = 3.5;
screw_hole_radius = 1.4;    // M3 self-tap pilot (2.8 mm) — enlarged so screws drive in.

lip_depth = 1.5;            // Downward lid lip/rabbet engagement depth.
lip_thickness = 1.5;
fit_clearance = 0.25;       // Small FDM-friendly clearance for lid fit.

// USB-C opening on the left end wall, centered on the actual connector.
// Jack edges observed on the print: top edge at Z = 13.5 (old slot top), left edge
// at Y = -3 (old slot's +Y / left wall). Jack ~9 x 3.2 mm, so its center is offset
// inward by half its size from those edges.
usbc_center_y = -3 - 9/2;   // = -7.5  (left edge -3, minus half the 9 mm width)
usbc_center_z = 13.5 - 3.2/2; // = 11.9  (top edge 13.5, minus half the 3.2 mm height)
usbc_slot_w   = 15;         // Y opening — clears a USB-C cable connector with margin.
usbc_slot_h   = 8.5;        // Z opening.

// Adafruit PDM MEMS mic breakout (#3492) in the lid, mic facing up (top-port
// MP34DT01-M). Mount from the ml_demo/enclosure mic test: pegs through the
// board's two mounting holes, heat-staked (peg tips melted over the board);
// the board rests flat on a seat ring round each peg, and a shallow pocket
// under the rest of the board takes the mic and its caps, leaving only
// mic_skin over the port.
//
// The lid prints letters-up, so the pegs can't stick out below it: the board
// sits in a recess in the lid's underside, deep enough that the peg tips end
// at the lid's bottom face. The pegs grow up from the bed, the seats are short
// columns on top of them, and the mic pocket roof is a ~14 mm bridge, so it
// still needs no supports.
mic_board_w    = 13.97;     // Along the hole line (0.55").
mic_board_h    = 12.7;      // Hole edge -> header edge (0.5").
mic_hole_inset = 2.54;      // Hole centres from the top and side edges.
mic_port_from_top = 5.6;    // Mic centre from the hole edge.
mic_pcb_t      = 1.6;
mic_chip_h     = 1.06;      // MP34DT01-M package; caps beside it are shorter.
mic_post_r     = 2.0;       // Seat (unpocketed lid) radius around each peg.
mic_pocket_margin = 0.3;    // Recess and pocket cover the whole board outline plus this.
mic_pocket_d   = 1.3;       // Mic pocket above the seats: ~0.24 mm over the mic.
mic_skin       = 1.2;       // Lid material over the pocket.
mic_peg_r      = 1.0;       // 2.0 mm peg through the board's ~2.5 mm mounting hole (2.3 printed too tight).
mic_peg_above  = 3.0;       // Peg past the back of the board, to heat-stake.
mic_audio_hole_r = 1.5;     // 3 mm audio hole through the top.
mic_audio_chamfer = 0.5;    // 45-deg chamfer on its outer edge.
// Board centre on the lid (assembled coords), front side, under "Word" and
// clear of the lettering, border, corner screws and locating-pin holes.
// mic_rot turns the board; at 0 the header edge faces +X.
mic_x   = 30;
mic_y   = -30;
mic_rot = 0;

// Corner bosses and lid screws.
boss_radius     = 4;
boss_hole_r     = 1.4;      // M3 self-tap pilot (2.8 mm) — enlarged so screws drive in.
boss_hole_depth = 12;
lid_screw_r     = 1.9;      // M3 clearance through lid (3.8 mm) — generous so the screw drops through.
gusset_width    = boss_radius * 3;  // Span of rectangular rib along each wall.

// Lid locating posts — chunky columns fused to the inside of the long walls, each
// with a pin on top that drops into a blind hole in the lid. They sit well inboard
// of the edge so the lid has material all the way around each hole (keeps the lid
// bottom flat — the holes are shallow blind pockets, not through-holes).
loc_col_r          = 3.5;   // Locating column radius.
loc_col_inset      = 2.0;   // Column center distance inboard from the inner wall face.
loc_pin_r          = 1.75;  // Pin radius (3.5 mm pin) — printable and sturdy.
loc_pin_h          = 2.0;   // Pin height above the wall rim.
loc_pin_hole_r     = 2.0;   // Lid hole radius = pin radius + clearance.
loc_pin_hole_depth = 2.5;   // Blind depth into the lid (leaves ~1 mm top skin).

label_line_1 = "Say the";
label_line_2 = "Magic Word";
label_font  = "Orbitron:style=Bold";
label_size  = 16;
label_line_pitch = 20;
label_height = 10;          // How far letters stand proud of the top surface.

// Magic-themed lid decorations (raised, like the lettering but lower).
deco_height       = 3;      // How far decorations stand proud of the top surface.
deco_border_inset = 0;      // Border frame flush with the lid edge, clear of the screw heads.
deco_border_width = 2;      // Border frame line width.

// Chest-style trim: bands wrapping the box, corner caps, and vertical straps.
strap_relief  = 1.8;        // How far bands/straps stand proud of the walls.
band_h        = 7;          // Bottom band height (stops just under the USB-C slot).
top_band_h    = 8;          // Top band height, flush with the rim.
strap_w       = 8;          // Vertical strap width.
strap_x       = 45;         // Vertical strap positions on front/back walls (+/-).
corner_cap_w  = 18;         // Corner cap leg length along each wall.

// Wall symbols (traced from the symbol pack, see magic_symbols.scad).
symbol_relief     = 1.2;    // How far symbols stand proud of the walls.
symbol_size_front = 40;     // Front medallion size.
symbol_size_back  = 36;
symbol_size_end   = 34;

// Through-holes in the upper part of the back wall (+Y side).
// (back_hole_z is set below, after base_height is defined.)
back_holes = [                    // Each entry is [x, diameter] along the back wall.
    [  25, 17.5 ],      // Switch mount.
    [ -25, 10.0 ],      // Wire pass-through.
];

part_gap = 14;              // Spacing between base and lid in default view.

// ---------------------------------------------------------------------------
// Derived dimensions
// ---------------------------------------------------------------------------

internal_length = pcb_length + 2 * pcb_clearance;
internal_width  = pcb_width  + 2 * side_clearance;
internal_height = standoff_height + pcb_height + height_above_pcb;

outer_length = internal_length + 2 * wall_thickness;
outer_width = internal_width + 2 * wall_thickness;
base_height = floor_thickness + internal_height;

back_hole_z = floor_thickness + standoff_height + pcb_height + 12;  // Hole centers 12 mm above the PCB top (z = 30.5), near the bottom like the original print.

lid_lip_length = internal_length - 2 * fit_clearance;
lid_lip_width = internal_width - 2 * fit_clearance;
lid_lip_outer_length = lid_lip_length;
lid_lip_outer_width = lid_lip_width;
lid_lip_inner_length = lid_lip_outer_length - 2 * lip_thickness;
lid_lip_inner_width = lid_lip_outer_width - 2 * lip_thickness;

outer_min_x = -outer_length / 2;
front_outer_y = -outer_width / 2;

// Mic stack in the lid, from its bottom face (z = 0) up: peg tip, board back,
// seat plane (board front), mic pocket roof, skin, lid top.
mic_board_back_z = mic_peg_above;
mic_seat_z       = mic_board_back_z + mic_pcb_t;
mic_pocket_top_z = mic_seat_z + mic_pocket_d;
lid_thickness    = max(lid_thickness_min, mic_pocket_top_z + mic_skin);   // 7.1 with the defaults.

// Mic features relative to the mic board centre (X = toward header edge).
mic_hole_dx = -(mic_board_h/2 - mic_hole_inset);
mic_hole_dy = mic_board_w/2 - mic_hole_inset;
mic_port_dx = mic_port_from_top - mic_board_h/2;

// ---------------------------------------------------------------------------
// Utility geometry
// ---------------------------------------------------------------------------

module rounded_rect_2d(length, width, radius) {
    offset(r = radius)
        square([length - 2 * radius, width - 2 * radius], center = true);
}

module rounded_box(length, width, height, radius) {
    linear_extrude(height = height)
        rounded_rect_2d(length, width, radius);
}

// PCB hole positions from (0,0) at bottom-left corner of board,
// converted to model coords (centered): subtract pcb_length/2 and pcb_width/2.
pcb_holes = [
    [ 8.0   - pcb_length/2,  5.0   - pcb_width/2 ],
    [ 12.6  - pcb_length/2,  58.5  - pcb_width/2 ],
    [ 106.0 - pcb_length/2,  5.0   - pcb_width/2 ],
    [ 113.1 - pcb_length/2,  58.5  - pcb_width/2 ],
];

module standoff_positions() {
    for (hole = pcb_holes)
        translate([hole[0], hole[1], 0])
            children();
}

module standoff() {
    difference() {
        cylinder(h = standoff_height, r = standoff_radius);
        translate([0, 0, -0.1])
            cylinder(h = standoff_height + 0.2, r = screw_hole_radius);
    }
}

module usbc_slot() {
    // Cut through the left end wall, centered on the USB-C jack.
    // Cube extends slightly past the outer face to avoid coincident-face artifacts.
    translate([
        -(outer_length / 2) + wall_thickness / 2,
        usbc_center_y,
        usbc_center_z
    ])
        cube([wall_thickness + 2 * strap_relief + 0.4, usbc_slot_w, usbc_slot_h], center = true);
}

// --- Lid mic mount ----------------------------------------------------------------
// Local frame: mic board centre at the origin, X toward the header edge, z = 0
// at the lid's bottom face. on_lid_mic() places it on the lid.

module on_lid_mic() {
    translate([mic_x, mic_y, 0]) rotate([0, 0, mic_rot]) children();
}

module mic_outline_2d() {
    square([mic_board_h + 2 * mic_pocket_margin, mic_board_w + 2 * mic_pocket_margin], center = true);
}

// Recess the board drops into from below, open at the bottom so the wires
// leave straight down into the box; then the mic pocket above the seat plane,
// leaving the seats as columns round the pegs.
module mic_pocket_cut() {
    translate([0, 0, -0.1])
        linear_extrude(mic_seat_z + 0.1) mic_outline_2d();
    translate([0, 0, mic_seat_z - 0.01])
        linear_extrude(mic_pocket_d + 0.01) difference() {
            mic_outline_2d();
            for (sy = [-1, 1]) translate([mic_hole_dx, sy * mic_hole_dy]) circle(r = mic_post_r + 0.05);
        }
}

// Pegs from the seats through the board holes to the lid's bottom face,
// flat-bottomed so the whole peg footprint sticks to the bed.
module mic_mount() {
    for (sy = [-1, 1])
        translate([mic_hole_dx, sy * mic_hole_dy, 0])
            cylinder(h = mic_seat_z + 0.01, r = mic_peg_r);
}

module mic_audio_hole() {
    translate([mic_port_dx, 0, mic_pocket_top_z - 0.1])
        cylinder(h = lid_thickness - mic_pocket_top_z + 0.2, r = mic_audio_hole_r);
    // 45-deg chamfer on the outer edge.
    translate([mic_port_dx, 0, lid_thickness - mic_audio_chamfer])
        cylinder(h = mic_audio_chamfer + 0.01, r1 = mic_audio_hole_r, r2 = mic_audio_hole_r + mic_audio_chamfer);
}

// Ghost of the mic board (and mic) for checking placement.
module mic_board_ghost() {
    translate([0, 0, mic_board_back_z])
        linear_extrude(mic_pcb_t) difference() {
            square([mic_board_h, mic_board_w], center = true);
            for (sy = [-1, 1]) translate([mic_hole_dx, sy * mic_hole_dy]) circle(r = 1.25);
        }
    translate([mic_port_dx - 1.5, -2, mic_seat_z]) cube([3, 4, mic_chip_h]);
}

// Returns the XY center of a corner boss given its signs.
function boss_pos(sx, sy) = [
    sx * (internal_length/2 - boss_radius),
    sy * (internal_width/2  - boss_radius)
];

// Locating-post centers — on the long walls, set inboard of the inner wall face so
// the column fuses to the wall but the pin/hole sits clear of the lid edge.
loc_col_xy = [
    [ 0,  (internal_width/2 - loc_col_inset) ],   // back long wall (+Y)
    [ 0, -(internal_width/2 - loc_col_inset) ],   // front long wall (-Y)
];

module locating_columns() {
    for (p = loc_col_xy)
        translate([p[0], p[1], floor_thickness]) {
            cylinder(h = internal_height, r = loc_col_r);     // column up to the rim
            translate([0, 0, internal_height])
                cylinder(h = loc_pin_h, r = loc_pin_r);       // pin above the rim
        }
}

// Horizontal holes through the back wall (+Y), for a switch and wire pass-through.
module back_wall_holes() {
    for (h = back_holes)
        translate([h[0], internal_width/2 - 0.1, back_hole_z])
            rotate([-90, 0, 0])
                cylinder(h = wall_thickness + strap_relief + 0.3, r = h[1] / 2);
}

module lid_label() {
    for (line = [
        [ label_line_1,  label_line_pitch / 2 ],
        [ label_line_2, -label_line_pitch / 2 ],
    ])
        translate([0, line[1], 0])
            text(
                line[0],
                size = label_size,
                font = label_font,
                halign = "center",
                valign = "center"
            );
}

// --- Magic-box lid decorations -----------------------------------------------

// Thin frame following the lid outline, passing inside the lid edge but
// outside the corner screw holes.
module deco_border_2d() {
    difference() {
        rounded_rect_2d(
            outer_length - 2 * deco_border_inset,
            outer_width  - 2 * deco_border_inset,
            corner_radius
        );
        rounded_rect_2d(
            outer_length - 2 * (deco_border_inset + deco_border_width),
            outer_width  - 2 * (deco_border_inset + deco_border_width),
            max(corner_radius - deco_border_width, 0.5)
        );
    }
}

// Lid decoration: a chest-lid rim frame around the lettering.
module lid_decorations_2d() {
    deco_border_2d();
}

// --- Wall placement helpers ----------------------------------------------------
// Each helper maps 2D children (u = along the wall, v = up from the bottom)
// onto a wall's exterior face, protruding `relief` with a 0.1 bite into the
// wall so the union is clean.

module on_front_wall(relief = strap_relief) {
    translate([0, front_outer_y + 0.1, 0])
        rotate([90, 0, 0])
            linear_extrude(relief + 0.1)
                children();
}

module on_back_wall(relief = strap_relief) {
    translate([0, -front_outer_y - 0.1, 0])
        rotate([-90, 0, 0])
            linear_extrude(relief + 0.1)
                mirror([0, 1])
                    children();
}

module on_left_wall(relief = strap_relief) {
    translate([outer_min_x + 0.1, 0, 0])
        rotate([0, 0, -90])
            rotate([90, 0, 0])
                linear_extrude(relief + 0.1)
                    mirror([1, 0])
                        children();
}

module on_right_wall(relief = strap_relief) {
    translate([-outer_min_x - 0.1, 0, 0])
        rotate([0, 0, 90])
            rotate([90, 0, 0])
                linear_extrude(relief + 0.1)
                    children();
}

// --- Chest trim ------------------------------------------------------------------

// A raised band wrapping continuously around all four walls (corners included).
module box_band(z0, h) {
    translate([0, 0, z0])
        linear_extrude(h)
            difference() {
                rounded_rect_2d(
                    outer_length + 2 * strap_relief,
                    outer_width  + 2 * strap_relief,
                    corner_radius + strap_relief
                );
                rounded_rect_2d(outer_length - 0.2, outer_width - 0.2, corner_radius);
            }
}

// Full-height caps hugging each vertical corner edge.
module corner_caps() {
    linear_extrude(base_height)
        intersection() {
            difference() {
                rounded_rect_2d(
                    outer_length + 2 * strap_relief,
                    outer_width  + 2 * strap_relief,
                    corner_radius + strap_relief
                );
                rounded_rect_2d(outer_length - 0.2, outer_width - 0.2, corner_radius);
            }
            union() {
                for (sx = [-1, 1])
                for (sy = [-1, 1])
                    translate([sx * outer_length / 2, sy * outer_width / 2])
                        square(2 * corner_cap_w, center = true);
            }
        }
}

module chest_trim() {
    box_band(0, band_h);
    box_band(base_height - top_band_h, top_band_h);
    corner_caps();

    // Vertical straps on the front and back walls.
    for (sx = [-1, 1]) {
        on_front_wall()
            translate([sx * strap_x - strap_w / 2, 0])
                square([strap_w, base_height]);
        on_back_wall()
            translate([sx * strap_x - strap_w / 2, 0])
                square([strap_w, base_height]);
    }
}

// Symbols from the pack, one per wall, centered between the straps/bands.
// offset() fattens the traced strokes ~0.5 mm so the raised lines print solidly.
module fat_symbol(size) {
    offset(r = 0.25)
        scale(size)
            children();
}

module wall_symbols() {
    on_front_wall(symbol_relief)
        translate([0, 42]) fat_symbol(symbol_size_front) symbol_front();
    on_back_wall(symbol_relief)
        translate([0, 48]) fat_symbol(symbol_size_back) symbol_back();  // Raised to clear the low back-wall holes.
    on_left_wall(symbol_relief)
        translate([0, 42]) fat_symbol(symbol_size_end) symbol_left();
    on_right_wall(symbol_relief)
        translate([0, 42]) fat_symbol(symbol_size_end) symbol_right();
}

module corner_bosses() {
    for (sx = [-1, 1])
    for (sy = [-1, 1])
        translate([boss_pos(sx,sy)[0], boss_pos(sx,sy)[1], 0])
            _boss_with_gussets(sx, sy);
}

module _boss_with_gussets(sx, sy) {
    difference() {
        union() {
            cylinder(h = internal_height, r = boss_radius);
            // Rib toward the end wall (X direction).
            translate([sx * boss_radius/2, 0, internal_height/2])
                cube([boss_radius, gusset_width, internal_height], center = true);
            // Rib toward the side wall (Y direction).
            translate([0, sy * boss_radius/2, internal_height/2])
                cube([gusset_width, boss_radius, internal_height], center = true);
        }
        // Blind M3 hole from the top.
        translate([0, 0, internal_height - boss_hole_depth])
            cylinder(h = boss_hole_depth + 0.1, r = boss_hole_r);
    }
}

// ---------------------------------------------------------------------------
// Printable parts
// ---------------------------------------------------------------------------

module base() {
    difference() {
        union() {
            difference() {
                rounded_box(outer_length, outer_width, base_height, corner_radius);

                translate([0, 0, floor_thickness])
                    rounded_box(
                        internal_length,
                        internal_width,
                        internal_height + 0.2,
                        max(corner_radius - wall_thickness, 0.1)
                    );
            }

            translate([0, 0, floor_thickness])
                standoff_positions()
                    standoff();

            translate([0, 0, floor_thickness])
                corner_bosses();

            locating_columns();

            chest_trim();
            wall_symbols();
        }

        usbc_slot();
        back_wall_holes();
    }
}

module lid() {
    difference() {
        union() {
            lid_shell();
            on_lid_mic() mic_mount();
        }
        on_lid_mic() mic_audio_hole();
    }
}

module lid_shell() {
    difference() {
        union() {
            // Flat top plate — the bottom face is flat (the mic pocket is a
            // recess with its pegs ending at the face) so the lid prints
            // letters-up with no supports.
            rounded_box(outer_length, outer_width, lid_thickness, corner_radius);

            // Raised lettering standing proud of the top surface.
            translate([0, 0, lid_thickness])
                linear_extrude(height = label_height)
                    lid_label();

            // Magic-box ornaments, raised lower than the lettering.
            translate([0, 0, lid_thickness])
                linear_extrude(height = deco_height)
                    lid_decorations_2d();
        }

        // M3 clearance holes aligned with corner bosses.
        for (sx = [-1, 1])
        for (sy = [-1, 1])
            translate([boss_pos(sx,sy)[0], boss_pos(sx,sy)[1], -0.1])
                cylinder(h = lid_thickness + 0.2, r = lid_screw_r);

        // Blind locating-pin holes in the bottom face (top stays flat).
        for (p = loc_col_xy)
            translate([p[0], p[1], -0.1])
                cylinder(h = loc_pin_hole_depth + 0.1, r = loc_pin_hole_r);

        on_lid_mic() mic_pocket_cut();
    }
}

module assembled() {
    base();

    translate([0, 0, base_height]) {
        lid();
        %on_lid_mic() mic_board_ghost();
    }
}

// Close-up of the lid mic mount, cut at cut_dy from the board centre
// (0 = through the audio hole, mic_hole_dy = through a peg).
module mic_section(cut_dy = 0) {
    intersection() {
        lid();
        translate([mic_x - 15, mic_y + cut_dy, -1]) cube([30, 15, 30]);
    }
    %on_lid_mic() mic_board_ghost();
}

// ---------------------------------------------------------------------------
// Default preview
// ---------------------------------------------------------------------------

base();

translate([0, outer_width + part_gap, 0])
    lid();

// Uncomment for assembled view:
// assembled();
