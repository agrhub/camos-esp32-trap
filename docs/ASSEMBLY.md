# Assembling the trap camera

![illustration of the mounted trap](img/trap-illustration.png)

*Illustration, not a photo.* Photos of an assembled trap will be added as `docs/img/trap-assembled.jpg` and the shots
listed at the end of this page.

Parts: [BOM.md](BOM.md). Wiring: [WIRING.md](WIRING.md). Flash and set up the camera on the bench first (README), and
take one snapshot from the web console before anything goes into the box.

## 1. Inside the box

The reference build uses the hanging solar trap housing from the BOM (item 3): the board, the modules and their wiring
are mounted inside its lid. The steps below also apply to a plain IP65 box (items 3a, 3b).

1. **Window.** Cut a 20 mm round hole in the side of the box that will face the card (or use a box with a clear lid and
   let the camera look through it). Glue a piece of clear acrylic over the hole from the inside with neutral silicone.
   Keep the acrylic clean and scratch-free: every smudge shows on the photos.
2. **Board.** Mount the Freenove board on a piece of plastic or perfboard with nylon standoffs so that the lens sits
   **centred behind the window, 2-5 mm from it**, square to the window. A lens touching the acrylic catches reflections;
   a lens far from it sees the edge of the hole.
3. **Camera ribbon.** Leave the OV5640 ribbon free of sharp bends. Fix the camera module with a dab of hot glue or
   double-sided foam so that it cannot tilt. **Focus** an adjustable lens (BOM item 1a) on the card: card at its final
   distance, Live View in the web console, turn the lens until the card's grid is sharp, then lock it (its lock ring or
   a drop of glue). A fixed lens focused far away (the Freenove kit's) is sharp only from about 50 cm.
4. **Field camera only:** place the LiPo flat at the bottom (foam tape), the charger and the prototype board with the
   5 V boost board and the screw terminals beside it, the 4G board against a side wall with its antenna (FPC antennas
   stick to the inside of the lid, away from the battery), the GPS board with its patch antenna **facing up** under the
   lid. Keep the GPS antenna 10 cm or more from the LTE antenna.
5. **Cables.** The USB or solar cable enters at the bottom (water runs down, not in), through the housing's own opening
   or a hole sealed with neutral silicone. Make a drip loop below it. Put a small bag of silica gel inside before
   closing.

## 2. Camera and card

- **Distance:** 15-25 cm from the lens to the sticky card. Move the card until it fills most of the live picture with
  a small margin (the field of view depends on the lens fitted to your OV5640 module).
- **Angle:** the lens square to the card (looking straight at it), centred. A slanted view blurs one side and stretches
  the insects.
- **Light:** shade the card and the window from direct sun (a small roof over both). Direct sun on the card makes it
  shine white; sun on the window flares. There is no infrared light: photos at night are black, so the default schedule
  takes photos in daylight only when you set a capture window (Configuration > Event > Capture Schedule).
- **Check the framing** in the web console: Live View at 1280 x 720 or 1600 x 1200, then Configuration > Image to adjust
  brightness or sharpness. Save, then take a snapshot ("Send to FlyBot") and look at it in FlyBot.
- Mount the card and the camera on the **same post or bracket**, so wind moves both together.

## 3. In the orchard

1. Post 1.2-1.5 m, in the shade of the canopy, where the farm normally hangs its fruit-fly traps.
2. The box on an arm, the card hanging on another arm in front of the window (see the illustration).
3. **Solar panel** (field camera): on top of the roof, facing the equator (south in Vietnam), tilted about 15 degrees,
   out of the canopy shade for as many hours as possible. Its cable runs down into the box through the bottom opening.
4. **Antennas:** the LTE antenna vertical; an external antenna above the roof is better than one inside the box when
   the signal is weak (the 4G page shows the signal).
5. Power on, wait for the LED: green blip = online. Check the camera in FlyBot (it uploads its first photo at the next
   capture slot) and on the 4G / Wi-Fi page of the web console.

## 4. Photos to take for this guide (shot list)

| File | What to show |
|---|---|
| `docs/img/trap-assembled.jpg` | the complete trap on its post in the orchard, front three-quarter view, panel and card visible |
| `docs/img/trap-front.jpg` | the box from the card's side: the window, the lens behind it, the roof |
| `docs/img/trap-inside.jpg` | the open box: board behind the window, battery, charger, prototype board, 4G and GPS boards, cables |
| `docs/img/trap-orchard.jpg` | the trap in its place among the trees (wider shot) |
| `docs/img/trap-card-view.jpg` | the sticky card as the camera sees it: a full-size snapshot downloaded from Playback |

JPEG, about 1600 px wide, under 300 KB each. No people's faces, house numbers or car plates in the frame.
