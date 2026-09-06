// hughes_bridge enclosure -- 2.8" ESP32-S3 (QDtech ES3C28P) + 1S LiPo bay.
// Parametric. Units: mm. Render one part at a time with `part`. See README.md.
//
// FORKED 2026-08-31 from intercom/intercom_case (same board), to inherit its
// screwless snap-rim retention + flush edge-to-edge bezel + wall stiffener + USB-C
// relief. Retuned for hughes: speaker OFF, the bigger 65mm AKZYTUE 103450 pack,
// pop-in battery clips, no mic hole. The speaker machinery is kept (speaker=false)
// so the ledger's future alarm-speaker mount is a one-flag flip away.
//
// KNOWN dims (vendor drawing ES3C28P_Size.pdf):
//   PCB 86.0 x 50.0 x 1.6, 4x M3 holes (Ø3.2) on a 78 x 42 rectangle,
//   LCD active 43.2 x 57.6 / visible 43.6 x 58.05, touch glass 50 x 69.2,
//   LCD stack ~4.3 proud of the PCB front, back SMD up to 4.7 below the PCB.
// UNKNOWN (not in the drawing -- MEASURE the board, or read ES3C28P_3D.step):
//   USB-C / BOOT / RESET / BAT-connector positions. They're parameters below.
//   VERIFIED from the STEP (2026-08-19): USB-C dead-centered on the -X edge (Y=0).

/* [Part to render] */
part = "both";              // [back, bezel, both, assembled, bezel_legacy]
                            //   back = shell to print; bezel = flush bezel to print; both = the two
                            //   side by side; assembled = bezel on the case (fit preview); bezel_legacy
                            //   = the original over-the-glass bezel (kept for reference).

/* [Debug] translucent board + connector markers over the render (preview only) */
show_board = false;

/* [Board] */
pcb_l   = 86.0;             // long axis (X)
pcb_w   = 50.0;             // short axis (Y)
pcb_t   = 1.6;
hole_dx = 78.0;            // M3 hole spacing, long axis
hole_dy = 42.0;            // M3 hole spacing, short axis
smd_back  = 4.7;           // tallest part on the PCB back
lcd_stack = 4.3;           // PCB front face -> touch-glass top
glass_l   = 69.2;          // cover-glass / touch-panel length (X) -- STEP-measured, centered on PCB
glass_w   = 50.0;          // cover-glass width (Y) = full PCB width (runs edge-to-edge on the long sides)

/* [Flush bezel] Alternative bezel (render part="bezel_flush"): the window fits AROUND the glass so
   the whole touch surface is exposed, the frame sits flush on the PCB front and stands flush_proud
   above the glass (protects the screen face-down), NO screws -- Ø pins center the board and the same
   long-wall snap rim holds it to the case. The standard bezel() is left untouched. */
flush_proud     = 1.0;     // frame stands this far above the glass top
flush_win_clr   = 0.5;     // window clearance around the glass (each side)
flush_win_r     = 1.0;     // window corner radius (glass is square; small so the corners clear)
flush_win_off_x = 0;       // window shift if a print shows it off-center (glass is centered -> 0)
flush_win_off_y = 0;
flush_pin_d     = 3.0;     // centering-pin Ø into the M3 holes (Ø3.2 -> ~0.2mm locate clearance)
flush_pin_h     = 1.6;     // pin length below the frame (= board thickness; fills the M3 hole)
flush_side_clr  = 0.4;     // slip-fit clearance between the frame and the cavity walls

/* [Screen window] The active area is NOT centered in the glass (MEASURED 2026-08-07):
   glass 69.5 x 50, dead border 8.4mm on the USB-C end and 2.5mm on the other three.
   So the lit area sits ~2.95mm = (8.4-2.5)/2 toward the FAR end. win_off_x shifts the
   window there, which extends the bezel over the dead glass on the USB-C side.
   Assumes the glass is centered on the board -- if not, fine-tune win_off_x against
   a printed bezel (+ = away from USB-C). */
win_l     = 58.6;          // active length  = 69.5 - 8.4 - 2.5
win_w     = 45.0;          // active width   = 50 - 2.5 - 2.5
win_off_x = 2.95;          // shift toward the far end (away from USB-C)
win_off_y = 0;

/* [Battery bay] AKZYTUE 2000mAh 103450 1S LiPo, MEASURED pack 65 x 36 x 10 (2026-08-07). With
   speaker OFF the pack centers on the board (bat_cx = bat_off_x), and it drives the case depth
   (floor_clear = bat_t, no speaker). It is 65mm long so its ends (±32.5) stop well short of the
   corner posts (±39) -- the posts never touch it and no post flats are needed. Held by the
   pop-in clips (see [Battery clips]); the old rails+end-stops are still available via bat_retain. */
bat_l = 65;                // battery length (X)
bat_w = 36;                // battery width (Y) -- clips add bat_clip_clr each side
bat_t = 10.5;              // battery thickness (Z) = 10mm measured + 0.5 swell margin; drives depth
bat_off_x = 0;             // manual nudge on top of the auto speaker-driven shift (see bat_cx)
bat_retain = false;        // battery end-stops + side rails. false = hold the cell with double-sided
                           //   tape instead (cleaner floor). true = print the locating lips.
bat_lead_end = -1;         // (only if bat_retain) end the wires exit: -1 = -X. Leads exit here.
bat_lead_gap = 16;         // width of the wire-exit notch in that end-stop
bat_lead_off_y = 12;       // shift the notch toward a corner (leads exit a corner, not center);
                           //   flip the sign if it's the other corner (verify in show_board)

/* [Battery clips] Pop-in retaining clips on the two long (±Y) sides so the cell snaps in and
   lifts out for replacement (no tape) -- same idea as the speaker hooks. They clear the corner
   posts in X (posts at ±39, clips well inside). 3 per side for the longer 65mm pack. */
bat_clips     = true;
bat_clip_clr  = 0.4;       // Y clearance between the cell and each clip
bat_clip_wall = 1.5;       // clip wall thickness (thin, so it flexes to snap)
bat_clip_w    = 6;         // each clip's width along X
bat_clip_over = 1.0;       // how far the catch overhangs the cell top
bat_clip_vclr = 0.4;       // vertical gap from the cell top to the catch
bat_clip_n    = 3;         // clips per long side (3 for the 65mm pack: -18.5 / 0 / +18.5)
bat_clip_inset_lo = 11;    // -X (wire/PCM-end) clips sit this far in from the cell's -X edge -- keeps
                           //   them off the protection board (PCM), which sits at the lead end
bat_clip_inset_hi = 11;    // +X clips sit this far in from the cell's +X edge (symmetric)

/* [Edge cutouts] USB-C MEASURED 2026-08-07; buttons still TODO */
usbc_end     = -1;         // which short edge the USB-C is on: -1 = -X end, +1 = +X end
usbc_off_y   = 0;          // Y offset from board center. STEP says centered -> 0
usbc_w       = 15.0;       // opening width  -- clears the 14mm cable overmold
usbc_h       = 10.0;       // opening height -- clears the 8.5mm cable overmold
usbc_z_below = 1.6;        // port center this far below the PCB back(component) face, toward floor
// The receptacle mouth sits ~3.2mm inside the outer face (2mm wall + 1.2mm side_gap), so a
// plug looks recessed. This flared relief removes outer wall around the port so the plug
// overmold noses in and seats near-flush. Set usbc_relief_d to the recess you measured
// (it can't exceed wall+side_gap ~3.2 without breaking through); it also eases plugging in.
usbc_relief   = true;
usbc_relief_w = 18;        // relief mouth width  (Y) -- clears the cable overmold + a seat
usbc_relief_h = 13;        // relief mouth height (Z)
usbc_relief_d = 1.8;       // outer wall removed (leaves ~1.4mm lip at the throat)
button_holes = false;      // set true once you've measured BOOT/RESET positions
boot_xy      = [30, -25];  // [x from center, y from center] -- placeholder
reset_xy     = [22, -25];  // placeholder
button_d     = 3.5;        // poke-hole Ø for a paperclip/stylus

/* [Speaker] Rectangular speaker firing OUT THE BACK -> grille in the floor. Body MEASURED
   40.5 x 28.5 x 9.5, R8 corners. Long (40.5) axis along Y (fits the 50mm width); 28.5 along
   X. Sits diaphragm-DOWN on the floor at the -X (USB/bottom) end. The battery shifts up (+X)
   to clear it, and the case grows a little at the +X (port-free) end -- both derived below.
   OFF for hughes_bridge (no alarm speaker fitted yet); flip true to add the ledger's alarm mount. */
speaker     = false;
spk_x       = 28.5;        // speaker body along X (case length)
spk_y       = 40.5;        // speaker body along Y (case width)
spk_t       = 9.5;         // speaker height (Z) -- sits on the floor
spk_r       = 8.0;         // corner radius (R8, radius-gauge measured)
spk_clr     = 0.4;         // clearance around the body inside the pocket
spk_wall    = 1.5;         // retaining-rib thickness around the pocket
spk_rib_h   = 6.0;         // retaining-rib height (< post_h so it clears the PCB)
spk_post_gap = 0.5;        // clearance between the speaker and the USB-end (-X) posts. The speaker
                           //   sits in the clear floor BETWEEN the two post rings (it can't overlap
                           //   the -X posts -- they hold the board), which sets how far the battery
                           //   shifts up and how long the case gets.
bat_spk_gap = 0.5;         // gap between the speaker pocket and the battery (-X end)
bat_top_clr = 0.5;         // gap between the battery +X end and the +X wall. With these tight, the
                           //   52mm cell fits inside the 53mm bat_floor, so the case stays ~97mm.
bat_floor   = 53;          // guaranteed OPEN floor length for the battery: speaker pocket +X wall
                           //   -> +X end wall. The case grows at +X to hit this (or more if a big
                           //   pack needs it). Board-only minimum is ~48mm; raise this for more room.
spk_holddown  = true;      // taller hooks on the two long walls that clip over the speaker top
spk_hook_over = 1.2;       // how far each hook overhangs the speaker top (the "catch")
spk_hook_w    = 9;         // hook width along the wall
spk_hook_clr  = 0.5;       // vertical gap from the speaker top to the catch. Print-tuned 2026-08-19:
                           //   was 0 (catch had to stay bent to grab); 0.5 lets it clip straight over.
spk_wire_gap  = 6;         // width of the wire-exit slots. Both short (±Y) walls get an OPEN,
                           //   full-height slot (no bridge) so the JST plug -- not just the bare
                           //   wire -- drops straight through. Widen if your connector needs it.

/* [Speaker grille] hole field through the floor under the driver, matched to the driver's
   EMITTING area -- a rounded rectangle inset from the speaker's outer edge by grille_inset
   on every side (so the grille frames the moving diaphragm, not the solid outer housing). */
grille_inset  = 2.0;       // emitting area is this far inside the speaker edge (each side)
grille_hole_d = 2.0;       // individual hole Ø
grille_pitch  = 3.6;       // hex spacing

/* [Mic hole] onboard MEMS mic (LMA2718B381). Body is on the board's component/back face
   but it's a bottom-port part venting through a hole on the board FRONT -> the sound path
   is front/bezel, so the hole goes through the bezel. XY from the ES3C28P STEP (PCB-centered,
   User_Library-MIC2716). Placed at the board XY directly in bezel() -- VERIFIED correct on a
   printed bezel (an earlier Y-mirror put it on the wrong side). */
mic_hole = false;         // OFF for hughes_bridge -- no mic in use, so no port hole in the bezel
mic_d    = 3.0;            // sound port Ø through the bezel
mic_xy   = [38.5, 15.0];  // from STEP: near the +X end, +Y long edge

/* [Bezel rim + snap] The flat lid didn't brace the walls, so the case bowed at the center.
   The bezel gets a LIP along the two long walls that nests into a rabbet cut in the wall
   tops -- it locates the bezel and ties the wall centers to it. Runs on the long walls only
   (the -X short wall carries the USB port). An optional snap bead/groove gives a gentle
   click; the click is clearance-sensitive, so tune rim_clr / snap_bead on a test print. */
bezel_rim  = true;
rim_h      = 5.0;          // how deep the lip reaches into the wall
lip_t      = 1.2;          // lip thickness -- 3 perimeters at 0.4mm line width (0.5 was 1 pass, too weak).
                           //   The lip protrudes ~0.25mm into the cavity to stay this thick without
                           //   thinning the standing outer wall; still clears the glass edge (~0.95mm).
rim_wall   = 1.6;          // outer wall left standing beside the lip (solid, ~4 perimeters with wall_y=3)
rim_clr    = 0.25;         // lip<->wall fit clearance (raise if too tight)
bezel_snap = true;         // add the snap bead + groove
snap_bead  = 0.40;         // detent size; interference = snap_bead - rim_clr. 0 = no click
snap_z     = 2.0;          // detent height up from the rabbet floor

/* [Case] */
wall       = 2.0;          // floor + short (USB-end) wall thickness
wall_y     = 3.0;          // LONG-side wall thickness (thicker so the bezel rabbet leaves a
                           //   solid standing wall, not a thin fin). Grows the case width outward
                           //   only; cavity/PCB fit is unchanged. Set = wall for the original slim sides.
side_gap   = 1.2;          // clearance between PCB edge and inner wall
smd_clear  = 0.8;          // gap under the back SMD
corner_r   = 3.0;
post_d     = 7.0;          // mounting-post diameter (M3 self-tap boss)
screw_shank = 3.4;         // M3 through-clearance (bezel + boss)
screw_head  = 6.0;         // M3 head counterbore Ø
screw_pilot = 2.8;         // M3 self-tap pilot into the post tops. 2.5 was too tight
                           //   (cammed out, chewed screw heads); 2.8 self-taps easily,
                           //   prints a hair under, still holds M3 well in PLA/PETG.
bat_side_clr = 0.5;        // clearance each side of the pack in Y (side rails)
bat_rail_h   = 6.0;        // height of the battery side rails
$fn = 48;

// ---- derived ----
inner_l = pcb_l + 2*side_gap;
inner_w = pcb_w + 2*side_gap;
floor_clear = max(bat_t, speaker ? spk_t : 0);  // tallest thing standing on the floor (speaker > battery now)
post_h  = floor_clear + smd_clear + smd_back;    // floor -> PCB back rest height (must clear that + the back SMD)
inner_h = post_h + pcb_t + lcd_stack;     // floor -> touch-glass top
outer_l = inner_l + 2*wall;
outer_w = inner_w + 2*wall_y;             // long (Y) walls use the thicker wall_y
outer_h = wall + inner_h;
pcb_back_z = wall + post_h;               // z of the PCB back (component/USB) face
usbc_z     = pcb_back_z - usbc_z_below;   // z of the USB-C opening center
pcb_front_z = pcb_back_z + pcb_t;         // z of the PCB front (glass-mounting) face
glass_top_z = pcb_front_z + lcd_stack;    // z of the touch-glass top

// ---- speaker + battery placement (derived) ----
// The speaker sits between the post rings; the battery tucks just past it on the floor. With the
// small cell both fit inside the board footprint, so spk_ext derives to 0 (no +X extension). A
// larger/longer pack would push bat_cx out and spk_ext would grow the +X end to fit it.
spk_foot_x = spk_x + 2*spk_clr + 2*spk_wall;                 // pocket outer length (X)
spk_off_x  = -hole_dx/2 + post_d/2 + spk_x/2 + spk_clr + spk_post_gap;  // just clear of the -X posts
spk_off_y  = 0;
spk_px     = spk_off_x + spk_foot_x/2;                       // speaker pocket +X wall (battery floor start)
bat_cx     = speaker ? spk_off_x + spk_foot_x/2 + bat_spk_gap + bat_l/2 + bat_off_x
                     : bat_off_x;                            // battery CENTER X (bat_off_x = nudge)
spk_ext    = speaker ? max(bat_cx + bat_l/2 + bat_top_clr - inner_l/2,   // room for the pack itself, and...
                           spk_px + bat_floor - inner_l/2,               // ...at least bat_floor of open floor
                           0) : 0;                                       // +X growth
bat_needs_flat = (bat_w + 2*bat_side_clr) > (hole_dy - post_d);  // pack wider than the +X post gap? then flat them

// ---- extended shell (grows only at +X; the -X/USB face stays put) ----
inner_l_e = inner_l + spk_ext;
outer_l_e = outer_l + spk_ext;
shell_dx  = spk_ext/2;                    // shift so the extra length lands at +X
rim_len   = inner_l_e - 2*(corner_r + 1); // lip/rabbet length along each long wall (clear of corners)

module rrect(l, w, h, r) { linear_extrude(h) hull() for (x=[-1,1], y=[-1,1]) translate([x*(l/2-r), y*(w/2-r)]) circle(r); }
module at_holes()  { for (x=[-1,1], y=[-1,1]) translate([x*hole_dx/2, y*hole_dy/2, 0]) children(); }

// Posts: solid Ø7 bosses from the floor up to the PCB rest. The board drops onto
// the tops; an M3 screw from the bezel passes through the board hole and self-taps
// into the pilot below. No fragile pin. Drawn from z=0 so they fuse with the floor,
// and unioned AFTER the cavity is cut -- otherwise the cavity eats them.
module posts() {
  at_holes() cylinder(h = wall + post_h, d = post_d);            // floor -> PCB rest
}
module post_pilots() {                                           // M3 pilot + funnel lead-in
  at_holes() {
    translate([0, 0, wall + post_h - 11]) cylinder(h = 11.1, d = screw_pilot);            // pilot
    translate([0, 0, wall + post_h - 1.2]) cylinder(h = 1.3, d1 = screw_pilot, d2 = screw_pilot + 2); // lead-in
  }
}
// Flats on the inner (battery-facing) Y face of the two +X posts. The pack is 36mm wide
// but the +X posts sit only 35mm apart, so as it slides up it would foul them; shaving
// ~1mm off each inner face opens a 37mm gap. Pilots stay centered/intact (~6mm of boss
// left, still self-taps M3). Only meaningful when the battery is shifted up (speaker on).
module post_flats() {
  fy = bat_w/2 + bat_side_clr;                                   // clearance plane |Y| = 18.5
  for (s = [-1, 1])
    translate([hole_dx/2, s*(fy - post_d/2), wall + post_h/2])
      cube([post_d + 2, post_d, wall + post_h + 2], center = true);
}
// Two end-stops that keep the 65mm pack from sliding along X (the side rails locate
// it in Y). One end carries a corner notch for the wires. Placed just outside the
// pack ends, clear of the corner posts. Drawn from the floor so they fuse to it.
module bat_fence() {
  h = min(bat_t, post_h - 0.6);
  for (s = [-1, 1])
    translate([bat_cx + s*(bat_l/2 + 1.5), 0, wall])
      difference() {
        translate([0, 0, h/2]) cube([2, bat_w + 3, h], true);
        if (s == bat_lead_end)                                  // wire-exit notch at the corner
          translate([0, bat_lead_off_y, h/2]) cube([4, bat_lead_gap, h + 2], true);
      }
}
// Y side-rails that locate the pack sideways. Needed because the corner posts sit
// at X=+-39, beyond the 65mm pack's ends (+-32.5), so they never touch it. Rails
// run along the pack with bat_side_clr each side; they clear the posts in X.
module bat_rails() {
  ry = bat_w/2 + bat_side_clr + 1;                              // rail centerline (1 = half of 2mm)
  for (s = [-1, 1])
    translate([bat_cx, s*ry, wall + bat_rail_h/2]) cube([bat_l, 2, bat_rail_h], true);
}
// Speaker pocket: a rounded-rect retaining ring on the floor that locates the back-firing
// speaker over the grille. The body drops in diaphragm-DOWN from the bezel side; secure it
// to the floor with adhesive/foam for an acoustic seal -- the PCB above leaves a gap and
// does NOT clamp it. Rib height stays under post_h so it clears the PCB.
module spk_pocket() {
  oy = spk_y/2 + spk_clr;                                       // pocket inner half-Y (short side)
  difference() {
    translate([spk_off_x, spk_off_y, wall])
      rrect(spk_x + 2*spk_clr + 2*spk_wall, spk_y + 2*spk_clr + 2*spk_wall, spk_rib_h, spk_r + spk_wall);
    translate([spk_off_x, spk_off_y, wall - 0.5])
      rrect(spk_x + 2*spk_clr, spk_y + 2*spk_clr, spk_rib_h + 1, spk_r);
    // wire-exit slots: full-height OPEN gaps through BOTH short (±Y) walls (no bridge), so the
    // JST plug drops straight in and the cable can exit either side.
    for (w = [-1, 1])
      translate([spk_off_x, spk_off_y + w*(oy + spk_wall/2), wall + (spk_rib_h + 1)/2])
        cube([spk_wire_gap, spk_wall*2 + 1, spk_rib_h + 1], center = true);
  }
  if (spk_holddown) for (s = [-1, 1]) spk_hook(s);              // clip-over hooks on the long walls
}
// One hold-down hook: a wall segment taller than the speaker on a long (±X) face, capped by
// an inward overhang whose flat underside (at z = speaker top) catches the speaker. The thin
// 1.5mm wall segment flexes out as the speaker presses in, then springs back over it.
// s = which long face (+1 / -1). A small top chamfer eases insertion.
module spk_hook(s) {
  ox    = spk_x/2 + spk_clr;                                    // pocket inner half-X (long side)
  xf    = spk_off_x + s*ox;                                     // long-wall inner face X
  catch = 1.0;                                                  // overhang thickness (Z)
  cz    = wall + spk_t + spk_hook_clr;                          // catch underside (just above the speaker top)
  h     = spk_t + spk_hook_clr + catch;                         // wall-segment height above floor top
  translate([xf + s*spk_wall/2, spk_off_y, wall + h/2])         // taller wall segment
    cube([spk_wall, spk_hook_w, h], center = true);
  hull() {                                                      // overhang: flat catch at cz, chamfered above
    translate([xf - s*spk_hook_over/2, spk_off_y, cz + 0.05])
      cube([spk_hook_over + spk_wall, spk_hook_w, 0.1], center = true);
    translate([xf + s*spk_wall/2, spk_off_y, cz + catch])
      cube([spk_wall, spk_hook_w, 0.1], center = true);
  }
}
// Grille through the floor under the driver: a hex hole field clipped to the emitting
// area -- a rounded rectangle inset grille_inset from the speaker footprint. Holes are
// kept only where they fall fully inside that field.
module grille() {
  gl = spk_x - 2*grille_inset;                                  // emitting field size (X)
  gw = spk_y - 2*grille_inset;                                  // emitting field size (Y)
  gr = max(0.1, spk_r - grille_inset);                          // its corner radius
  m  = grille_hole_d/2 + 0.3;                                   // margin: hole stays inside
  L = gl - 2*m; W = gw - 2*m; R = max(0.01, gr - m);            // field shrunk by the margin
  ni = ceil(gl / grille_pitch) + 1;
  nj = ceil(gw / grille_pitch) + 1;
  translate([spk_off_x, spk_off_y, 0])
    for (i = [-ni:ni], j = [-nj:nj]) {
      x = i * grille_pitch;
      y = j * grille_pitch + (i % 2) * grille_pitch/2;          // hex row offset
      dx = max(abs(x) - (L/2 - R), 0);
      dy = max(abs(y) - (W/2 - R), 0);
      if (abs(x) <= L/2 && abs(y) <= W/2 && sqrt(dx*dx + dy*dy) <= R)   // inside rounded rect
        translate([x, y, -1]) cylinder(h = wall + 2, d = grille_hole_d);
    }
}

// Battery pop-in clips: thin wall segments on the ±Y sides that snap over the cell top,
// analogous to spk_hook. bat_clip_n per side, spread along the cell length.
module bat_clip(sy, cx) {
  cyf   = sy*(bat_w/2 + bat_clip_clr);                         // clip inner face Y
  catch = 0.8;                                                 // overhang thickness (Z)
  cz    = wall + bat_t + bat_clip_vclr;                        // catch underside (just above the cell)
  h     = bat_t + bat_clip_vclr + catch;                       // wall-segment height above floor
  translate([cx, cyf + sy*bat_clip_wall/2, wall + h/2])        // taller wall segment
    cube([bat_clip_w, bat_clip_wall, h], center = true);
  hull() {                                                     // catch: tip at cyf-over (grip = over-clr), chamfered top
    translate([cx, cyf - sy*bat_clip_over/2, cz + 0.05])
      cube([bat_clip_w, bat_clip_over, 0.1], center = true);
    translate([cx, cyf + sy*bat_clip_wall/2, cz + catch])
      cube([bat_clip_w, bat_clip_wall, 0.1], center = true);
  }
}
module bat_clips_all() {
  lo = bat_cx - bat_l/2 + bat_clip_inset_lo + bat_clip_w/2;    // -X (speaker-end) clip center
  hi = bat_cx + bat_l/2 - bat_clip_inset_hi - bat_clip_w/2;    // +X clip center
  xs = (bat_clip_n <= 1) ? [(lo + hi)/2]
                         : [for (i = [0:bat_clip_n-1]) lo + i*(hi - lo)/(bat_clip_n - 1)];
  for (sy = [-1, 1], x = xs) bat_clip(sy, x);
}

module usbc_cut() {
  translate([usbc_end*(outer_l/2), usbc_off_y, usbc_z])          // outer_l (not extended): -X wall stays put
    cube([2*wall+2, usbc_w, usbc_h], center = true);
  // Flared outer relief: a large rect at the outer face tapering to the through-hole a
  // little way in, so the plug seats near-flush instead of down a deep slot.
  if (usbc_relief)
    translate([usbc_end*(outer_l/2), usbc_off_y, usbc_z])
      hull() {
        translate([usbc_end*0.2, 0, 0]) cube([0.4, usbc_relief_w, usbc_relief_h], center = true);       // flared mouth at the face
        translate([-usbc_end*usbc_relief_d, 0, 0]) cube([0.4, usbc_w + 1, usbc_h + 1], center = true);  // throat, usbc_relief_d in
      }
}

// Rabbet in the long-wall tops (case side): removes the inner rim_t of each long wall for
// the top rim_h so the bezel lip can nest in, plus an optional snap groove in the standing
// outer wall. Subtracted from the shell. Long walls only -- the -X wall carries the USB port.
module wall_rabbet() {
  sw_in  = outer_w/2 - rim_wall;                                    // standing-wall inner face
  cut_in = inner_w/2 - 0.6;                                         // rabbet reaches this far into the cavity
  cy = (sw_in + cut_in)/2; cw = sw_in - cut_in;                     // rabbet band center / width
  for (s = [-1, 1]) {
    translate([shell_dx, s*cy, outer_h - rim_h/2 + 0.5])
      cube([rim_len, cw, rim_h + 1], center = true);
    if (bezel_snap)                                                 // groove in the standing outer wall
      translate([shell_dx, s*(sw_in + snap_bead/2), outer_h - rim_h + snap_z])
        cube([rim_len - 4, snap_bead + 0.02, snap_bead*2], center = true);
  }
}
// Bezel lip (bezel side): bars that fill the rabbet, drawn on the +z (boss) side so they
// point into the case after the bezel flips over. Optional snap bead on the outer face.
module bezel_lip() {
  sw_in = outer_w/2 - rim_wall;                                     // standing-wall inner face
  lo = sw_in - rim_clr;                                             // lip outer face
  lc = lo - lip_t/2;                                                // lip center Y
  for (s = [-1, 1]) {
    translate([shell_dx, s*lc, wall + rim_h/2])
      cube([rim_len - 2*rim_clr, lip_t, rim_h], center = true);
    if (bezel_snap)                                                 // bead that clicks into the wall groove
      translate([shell_dx, s*(lo + snap_bead/2 - 0.01), wall + rim_h - snap_z])
        cube([rim_len - 4 - 2*rim_clr, snap_bead + 0.02, snap_bead*2], center = true);
  }
}

module back_shell() {
  union() {
    difference() {                                    // hollow shell + wall cutouts
      translate([shell_dx, 0, 0]) rrect(outer_l_e, outer_w, outer_h, corner_r);
      translate([shell_dx, 0, wall]) rrect(inner_l_e, inner_w, inner_h+1, corner_r-0.5);
      usbc_cut();
      if (speaker) grille();                          // rear speaker grille through the floor
      if (bezel_rim) wall_rabbet();                   // rabbet (+snap groove) in the long-wall tops
      if (button_holes) for (p = [boot_xy, reset_xy])
        translate([p[0], p[1], -1]) cylinder(h = wall+2, d = button_d);
    }
    difference() {                                    // posts + stops + rails + pocket, fused to the floor
      union() { posts(); if (bat_retain) { bat_fence(); bat_rails(); } if (speaker) spk_pocket(); if (bat_clips) bat_clips_all(); }
      post_pilots();
      if (speaker && bat_needs_flat) post_flats();    // shave the +X posts only if the pack is too wide
    }
  }
}

// Bezel: printed plate-down, screen window through it, and four Ø7 clamp-bosses on
// the TOP face (print orientation). In assembly the bezel flips over, the bosses
// point down and press the board onto the posts, and the M3 heads sit in the
// counterbores on the (now-up) plate face. boss_h fills the board-top -> bezel gap.
// The plate extends to cover the +X speaker extension and rests on the extended rim.
boss_h = max(0.1, lcd_stack - wall);
module bezel() {
  difference() {
    union() {
      translate([shell_dx, 0, 0]) rrect(outer_l_e, outer_w, wall, corner_r);   // plate (extended)
      at_holes() translate([0,0,wall]) cylinder(h = boss_h, d = post_d);        // clamp-bosses (print-up)
      if (bezel_rim) bezel_lip();                                              // long-wall nesting lip (+snap)
    }
    translate([win_off_x, win_off_y, -1]) rrect(win_l, win_w, wall+boss_h+2, 2);  // screen window
    at_holes() {
      translate([0,0,-1]) cylinder(h = wall + boss_h + 2, d = screw_shank);  // M3 through-clearance
      // 90° countersink for a flat-head M3 -- seats flush in the thin plate.
      // Ø screw_head at the surface, tapering to the shank; overshoot 0.3 for a clean cut.
      translate([0,0,-0.3]) cylinder(h = (screw_head-screw_shank)/2 + 0.3, d1 = screw_head + 0.6, d2 = screw_shank);
    }
    // Mic sound port. VERIFIED against a printed bezel (2026-08-19): the hole lands on the
    // correct side placed at the board Y directly (the earlier Y-mirror was wrong for how the
    // bezel actually seats). If a future change flips the bezel's parity, flip this Y sign.
    if (mic_hole)
      translate([mic_xy[0], mic_xy[1], -1]) cylinder(h = wall + boss_h + 2, d = mic_d);
  }
}

// Preview-only ghost of the real board, so cutouts can be eyeballed against it.
// Green = PCB, blue = screen glass, red = USB-C port + cable stub. Colours/alpha
// show in OpenSCAD preview (F5). Keep show_board=false for STL export.
// Flush bezel -- EDGE-TO-EDGE stepped. A 1mm-proud cap runs over the whole footprint (flush with the
// case outer edge, covering the wall tops so a face-down drop lands on the full rim), and an inner
// skirt steps down to sit flush on the PCB front and press the board. Window around the glass; no
// screws -- Ø pins center the board and the SAME long-wall snap rim as the standard bezel holds it.
// bezel_flush_asm() is the ASSEMBLY orientation (used by part="assembled"); bezel_flush() flips it to
// PRINT orientation (face-down, cap on the bed) so it prints like the standard bezel and the mic
// lands on the print-verified-correct side.
module bezel_flush_asm() {
  fc   = flush_side_clr;
  fz0  = pcb_front_z;                           // skirt back -- rests flush on the PCB front
  ftop = glass_top_z + flush_proud;             // proud top, over the whole footprint
  th   = ftop - fz0;                            // window-cut height
  difference() {
    union() {
      // 1mm-proud cap over the FULL footprint -- covers the wall tops, flush with the case edge
      translate([shell_dx, 0, glass_top_z]) rrect(outer_l_e, outer_w, flush_proud, corner_r);
      // inner skirt: PCB front -> glass top, inner-sized -- steps down to press the board flat
      translate([shell_dx, 0, fz0]) rrect(inner_l_e - 2*fc, inner_w - 2*fc, glass_top_z - fz0, corner_r - 0.5);
      if (bezel_rim) bezel_flush_lip(fz0, glass_top_z - fz0);   // long-wall snap lip into the rabbet
    }
    translate([flush_win_off_x, flush_win_off_y, fz0 - 1])                     // window around the glass
      rrect(glass_l + 2*flush_win_clr, glass_w + 2*flush_win_clr, th + 2, flush_win_r);
    // mic: board Y is mirrored here (this frame is un-flipped); the flip in bezel_flush() lands it on
    // the same side as the standard bezel's print-verified hole.
    if (mic_hole)
      translate([mic_xy[0], -mic_xy[1], fz0 - 1]) cylinder(h = th + 2, d = mic_d);
  }
  at_holes() translate([0, 0, fz0 - flush_pin_h]) cylinder(h = flush_pin_h + 0.01, d = flush_pin_d); // centering pins
}
// Print orientation (face-down, cap on the bed / pins up) -- what part="bezel" outputs.
module bezel_flush() {
  translate([0, 0, glass_top_z + flush_proud]) rotate([180, 0, 0]) bezel_flush_asm();
}
// Snap lip for the flush bezel: bars on the long sides bridging the frame edge out into the wall
// rabbet, plus the snap bead at the wall-groove height. Assembly orientation (no flip).
module bezel_flush_lip(fz0, th) {
  lip_in  = inner_w/2 - flush_side_clr - 0.5;   // overlap the frame slab
  lip_out = outer_w/2 - rim_wall - rim_clr;     // lip outer face (nests in the rabbet)
  lc = (lip_in + lip_out)/2; lw = lip_out - lip_in;
  for (s = [-1, 1]) {
    translate([shell_dx, s*lc, fz0 + th/2])
      cube([rim_len, lw, th], center = true);
    if (bezel_snap)                             // bead into the wall groove (same height as wall_rabbet)
      translate([shell_dx, s*(lip_out + snap_bead/2 - 0.01), outer_h - rim_h + snap_z])
        cube([rim_len - 4, snap_bead + 0.02, snap_bead*2], center = true);
  }
}

module board_ghost() {
  color([0.3,0.8,0.3,0.35])                                            // PCB (centered on X,Y)
    translate([0, 0, pcb_back_z + pcb_t/2]) cube([pcb_l, pcb_w, pcb_t], true);
  // GREY = the touch glass (69.5x50, assumed board-centered). BLUE = the lit area,
  // off-center within it (2.95 toward the far end). The window should frame the BLUE.
  color([0.5,0.5,0.5,0.25])
    translate([0, 0, pcb_front_z + lcd_stack/2]) cube([glass_l, glass_w, lcd_stack], true);
  color([0.3,0.5,1.0,0.45])
    translate([win_off_x, win_off_y, pcb_back_z+pcb_t+lcd_stack/2]) cube([58.6, 45, lcd_stack], true);
  // RED = the USB-C plug metal, standard 8.3x3.2 (same on every cable). Its
  // CENTER must sit at the middle of the wall hole -> usbc_z is right.
  color([1.0,0.2,0.2,0.7])
    translate([usbc_end*(pcb_l/2), usbc_off_y, usbc_z]) cube([16, 8.3, 3.2], true);
  // YELLOW = the measured cable overmold, 14x8.5. It must clear the 15x10 hole.
  color([1.0,0.85,0.1,0.30])
    translate([usbc_end*(outer_l/2+6), usbc_off_y, usbc_z]) cube([16, 14, 8.5], true);
  // ORANGE = the LiPo pack on the floor, shifted up to bat_cx. Should sit between the
  // side rails (Y) and between the end-stops (X), under the PCB with a hair of clearance.
  color([1.0,0.6,0.1,0.30])
    translate([bat_cx, 0, wall + bat_t/2]) cube([bat_l, bat_w, bat_t], true);
  // PURPLE = the rear speaker on the floor at the -X end, firing out the grille (down).
  if (speaker)
    color([0.6,0.3,0.9,0.35])
      translate([spk_off_x, spk_off_y, wall + spk_t/2]) cube([spk_x, spk_y, spk_t], true);
  // CYAN = the onboard mic, on the board's component face, venting front through the bezel.
  if (mic_hole)
    color([0.2,0.8,0.9,0.8])
      translate([mic_xy[0], mic_xy[1], pcb_back_z - 0.5]) cube([2.7, 1.9, 1.0], true);
}

module scene() {
  back_shell();
  if (show_board) translate([0,0,0]) board_ghost();
}

if (part == "back")  scene();                                     // back shell (print)
else if (part == "bezel") bezel_flush();                          // flush edge-to-edge bezel (print)
else if (part == "assembled") { back_shell(); bezel_flush_asm(); if (show_board) board_ghost(); }  // fit preview
else if (part == "bezel_legacy") bezel();                         // original over-the-glass bezel (kept)
else { scene(); translate([0, outer_w + 6, 0]) bezel_flush(); }   // "both": back + flush bezel, side by side
