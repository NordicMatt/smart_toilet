/*
  Parametric enclosure for nRF54LM20DK project box.

  Default view renders base and lid separated for printing/inspection.
  Use:
    base();
    lid();
    assembled();
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
lid_thickness = 3.5;        // Thicker top gives room for blind locating-pin pockets.
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

// Interior front microphone breakout holder.
mic_board_w = 14;           // Mic breakout PCB width (X). VERIFY against actual board.
mic_board_slot_t = 3.0;     // Slot thickness (Y) — roomy fit so the board slides easily and wires have space.
mic_grip = 2.5;             // How much each side rail overlaps the board edge (X, per side).
mic_slot_depth = 18;        // How far down from the tower top the board slides.
mic_relief_t = 2.5;         // Relief pocket depth (Y) between front wall and board center, so the mic housing floats with an air gap.
mic_audio_hole_r = 1.5;     // Front audio hole radius (3 mm diameter).
mic_tower_wall = 2.5;       // Tower wall thickness.
mic_capsule_z_offset = 6;   // Audio hole height above slot bottom; adjust per actual mic capsule layout.
mic_tower_x = 30;           // Clears center locating column, corner bosses, and nearby PCB standoff/front PCB edge.
mic_wire_cut_w = 12;        // Wire exit slot width through the holder's back wall, open at the top so wires drop in.

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
front_inner_y = -internal_width / 2;

mic_tower_w = mic_board_w + 2 * mic_tower_wall;
mic_tower_depth = mic_relief_t + mic_board_slot_t + mic_tower_wall;
mic_board_channel_w = mic_board_w + 0.5;
mic_relief_w = mic_board_channel_w - 2 * mic_grip;
mic_slot_bottom_z = base_height - mic_slot_depth;
mic_slot_cut_h = mic_slot_depth + 0.4;
mic_slot_cut_z = mic_slot_bottom_z - 0.1 + mic_slot_cut_h / 2;
mic_tower_overlap = 0.05;
mic_slot_wall_bite = 0.1;   // Relief pocket bites slightly into the wall face so the pocket front is exactly the wall.
mic_audio_hole_z = floor_thickness + internal_height - mic_slot_depth + mic_capsule_z_offset;

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

module mic_holder() {
    translate([
        mic_tower_x,
        front_inner_y + mic_tower_depth / 2 - mic_tower_overlap / 2,
        floor_thickness + internal_height / 2 - mic_tower_overlap / 2
    ])
        cube(
            [
                mic_tower_w,
                mic_tower_depth + mic_tower_overlap,
                internal_height + mic_tower_overlap
            ],
            center = true
        );
}

module mic_holder_cuts() {
    // Relief pocket against the front wall, leaving side rails to grip PCB edges.
    translate([
        mic_tower_x,
        front_inner_y + (mic_relief_t - mic_slot_wall_bite) / 2,
        mic_slot_cut_z
    ])
        cube([mic_relief_w, mic_relief_t + mic_slot_wall_bite, mic_slot_cut_h], center = true);

    // Board channel: open from the top, just behind the relief pocket.
    translate([
        mic_tower_x,
        front_inner_y + mic_relief_t + mic_board_slot_t / 2,
        mic_slot_cut_z
    ])
        cube([mic_board_channel_w, mic_board_slot_t, mic_slot_cut_h], center = true);

    // Audio path through the front wall into the relief pocket.
    translate([
        mic_tower_x,
        front_outer_y + wall_thickness / 2,
        mic_audio_hole_z
    ])
        rotate([90, 0, 0])
            cylinder(h = wall_thickness + 0.4, r = mic_audio_hole_r, center = true);

    // Wire exit slot through the holder's back wall into the box interior —
    // open all the way to the top so wires drop in instead of threading
    // through a hole. The side rails still capture the board; the lid closes
    // over the slot. Starts 1 mm below the slot bottom.
    wire_cut_h = base_height - (mic_slot_bottom_z - 1) + 0.4;
    translate([
        mic_tower_x,
        front_inner_y + mic_relief_t + mic_board_slot_t + (mic_tower_wall + 0.5) / 2 - 0.25,
        mic_slot_bottom_z - 1 + wire_cut_h / 2
    ])
        cube([mic_wire_cut_w, mic_tower_wall + 0.5, wire_cut_h], center = true);
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

            mic_holder();

            chest_trim();
            wall_symbols();
        }

        usbc_slot();
        back_wall_holes();
        mic_holder_cuts();
    }
}

module lid() {
    difference() {
        union() {
            // Flat top plate — the bottom face is completely flat so the lid
            // prints letters-up with no supports.
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
    }
}

module assembled() {
    base();

    translate([0, 0, base_height])
        lid();
}

// ---------------------------------------------------------------------------
// Default preview
// ---------------------------------------------------------------------------

base();

translate([0, outer_width + part_gap, 0])
    lid();

// Uncomment for assembled view:
// assembled();
