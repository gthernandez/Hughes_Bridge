// hughes_bridge protective scabbard -- a reversible slide-in sleeve for the
// FINISHED hughes_case (bezel + back shell). The assembled case slides in:
//   * USB-end FIRST  -> the plug seats in the closed bottom (covered)  [transport]
//   * flip 180 deg   -> USB sits at the open mouth (reachable to charge) [in use]
// The screen is covered by a broad wall either way. Units: mm.
//
// This WRAPS the printed case -- there is NO case reprint. It works reversibly
// because both short ends share the same outer outline (the USB opening is just a
// recess) and the USB faces along the slide axis.
//
// REBASED for the REWORKED case (screwless snap-rim fork), then MEASURED with calipers
// (snug) on the printed assembled case 2026-09-01: 92.25 L x 58.25 W x 25.25 D, bezel
// included. The case grew ~2mm wider than the old screw case (wall_y=3 stiffener), and
// the flush bezel seats LOWER than the old screw bezel -- so depth came in at 25.25, NOT
// the 26.6 first estimated by carrying the old case's +1.70 print delta forward. Trust
// the calipers. (For reference the model-nominal envelope is 92.4 x 58.4 x 24.9.)
//
// Render: open in OpenSCAD, F5 preview / F6 render, Export as STL. One part.
// Headless (like the case): QT_QPA_PLATFORM=offscreen openscad -o scab.stl hughes_scabbard.scad

/* [Render] */
show_case = false;    // translucent stand-in of the inserted case (preview only, not printed)

/* [Assembled case -- what slides in] MEASURED on the printed case, 2026-08-11 */
case_l = 92.25;       // long / insertion axis -- MEASURED (calipers, snug, 2026-09-01)
case_w = 58.25;       // width (short axis)    -- MEASURED (calipers, snug, 2026-09-01)
case_d = 25.25;       // depth, bezel included -- MEASURED (calipers, snug, 2026-09-01)
case_r = 0.0;         // case long-edge corner radius -- MEASURED SQUARE on the printed
                      //   case. A rounded cavity corner blocks a square case corner, so
                      //   the CAVITY corners track this (in_r = case_r + clr); the OUTER
                      //   edge is rounded separately via out_round below.

/* [Fit] MEASURE against a printed test slice before the full print (see case README).
   0.35 = snug/grippy, 0.4 = medium (default), 0.5 = loose/drops in. */
clr     = 0.4;        // slide clearance, per face
wall    = 2.0;        // side wall thickness
floor_t = 2.0;        // closed-end (bottom) thickness
proud   = 3.0;        // how far the case stands out of the mouth, for grip
out_round = 3.0;      // OUTER edge radius -- hold/feel only, independent of the square
                      //   inner cavity, so a square-cornered case still slides in

/* [Thumb relief] a finger scoop at the mouth on BOTH broad faces so you can
   pinch the case and push it out -- symmetric, so it serves either orientation. */
scoop    = true;
scoop_r  = 7.0;       // scoop radius (depth down from the mouth)
scoop_w  = 28.0;      // scoop width, along the case width

/* [Detente] a light friction rib just inside the mouth, both broad faces, so the
   case will not fall out when the sleeve is inverted, yet still pulls free by hand.
   Rib rides the broad faces -- clear of the USB opening on the short end. */
detente     = true;
det_h       = 0.7;    // rib penetration PAST the cavity wall. The case floats clr off the
                      //   wall, so the NET bite against the case is (det_h - clr). Keep
                      //   det_h > clr or the rib grabs nothing. Net 0.2-0.4 in PLA is a
                      //   light catch; here 0.7 - 0.4 = 0.30 net.
det_r       = 1.0;    // rib half-round radius (dia <= wall, stays buried on the outside)
det_below   = 9.0;    // rib center this far below the mouth rim

$fn = 64;
eps = 0.02;

// ---- derived ----
in_x  = case_w + 2*clr;     // cavity width
in_y  = case_d + 2*clr;     // cavity depth
in_r  = case_r + clr;       // cavity corner radius
cav_z = case_l - proud;     // cavity depth: mouth rim -> inner floor
out_x = in_x + 2*wall;      // outer width
out_y = in_y + 2*wall;      // outer depth
out_r = out_round;          // outer corner radius (independent of the square inner)
out_z = cav_z + floor_t;    // overall height

// rounded rectangle (rounds the 4 vertical/long edges, matching the case)
module rrect(x, y, r) { offset(r) offset(-r) square([x, y], center = true); }
module rrect_prism(x, y, z, r) { linear_extrude(height = z) rrect(x, y, r); }

module thumb_scoops() {
    for (s = [1, -1])
        translate([0, s * out_y/2, out_z])
            rotate([0, 90, 0])
                cylinder(h = scoop_w, r = scoop_r, center = true);
}

module detente_ribs() {
    for (s = [1, -1])
        translate([0, s * (in_y/2 + det_r - det_h), out_z - det_below])
            rotate([0, 90, 0])
                cylinder(h = in_x - 8, r = det_r, center = true);
}

module scabbard() {
    union() {
        difference() {
            rrect_prism(out_x, out_y, out_z, out_r);              // outer solid
            translate([0, 0, floor_t])
                rrect_prism(in_x, in_y, cav_z + eps + 10, in_r);  // cavity, open at the mouth
            if (scoop) thumb_scoops();
        }
        if (detente) detente_ribs();
    }
}

// preview-only: the inserted case (transport orientation, USB in the sealed end)
module case_ghost() {
    color([0.20, 0.72, 0.78, 0.32])
        translate([0, 0, floor_t])
            rrect_prism(case_w, case_d, case_l, case_r);
    color([0.90, 0.52, 0.20, 0.85])                               // USB-C, in the covered end
        translate([0, 0, floor_t + 5])
            rrect_prism(15, in_y * 0.68, 9, 1);
}

scabbard();
if (show_case) case_ghost();
