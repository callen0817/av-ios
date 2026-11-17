import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from matplotlib.patches import FancyArrowPatch, Circle, Wedge, Rectangle
import numpy as np
import os

os.makedirs('docs/assets', exist_ok=True)

fig = plt.figure(figsize=(16, 9), facecolor='#0D1117')
fig.suptitle('ARTIFICIAL VISION — MOBILE HARDWARE RIG: 90° FORWARD-FACING MOUNT SCHEMATIC',
             fontsize=17, fontweight='bold', color='#E5C983', y=0.96)

# Subplot 1: Side Elevation View (Profile)
ax1 = fig.add_axes([0.05, 0.10, 0.40, 0.80], facecolor='#161B22')
ax1.set_title('SIDE ELEVATION (PROFILE VIEW)\nDome Facing Forward · Cable Facing Down',
              fontsize=13, fontweight='bold', color='#FCFBF8', pad=12)

# iPad Body (Profile)
ipad_x = 2.0
ipad_y = 1.0
ipad_w = 0.35  # thickness ~7mm scaled
ipad_h = 7.0   # height ~250mm scaled
ipad_rect = Rectangle((ipad_x, ipad_y), ipad_w, ipad_h,
                      facecolor='#21262D', edgecolor='#8B949E', linewidth=2, zorder=2)
ax1.add_patch(ipad_rect)
ax1.text(ipad_x + ipad_w/2, ipad_y + ipad_h*0.45, 'iPad 10th Gen / Air\nChassis Profile',
         color='#8B949E', fontsize=10, ha='center', va='center', rotation=90)

# iPad Rear Camera (Back of iPad, near top)
cam_x = ipad_x
cam_y = ipad_y + ipad_h - 0.7
cam_w = 0.20
cam_h = 0.35
cam_rect = Rectangle((cam_x - cam_w, cam_y), cam_w, cam_h,
                     facecolor='#30363D', edgecolor='#BAA164', linewidth=1.5, zorder=3)
ax1.add_patch(cam_rect)
ax1.text(cam_x - cam_w - 0.1, cam_y + cam_h/2, 'iPad Camera Lens',
         color='#E5C983', fontsize=9.5, fontweight='bold', ha='right', va='center')

# Camera Forward Optical Axis (+Z_cam)
cam_arrow = FancyArrowPatch((cam_x - cam_w, cam_y + cam_h/2), (cam_x - cam_w - 2.2, cam_y + cam_h/2),
                            arrowstyle='-|>', mutation_scale=18, color='#38BDF8', linewidth=2.5, zorder=5)
ax1.add_patch(cam_arrow)
ax1.text(cam_x - cam_w - 1.2, cam_y + cam_h/2 + 0.25, 'Camera Line-of-Sight (+Z_cam FORWARD)',
         color='#38BDF8', fontsize=9.5, fontweight='bold', ha='center')

# Precision Mount Bracket (Clamping on top of iPad)
bracket_x = ipad_x - 0.25
bracket_y = ipad_y + ipad_h - 0.2
bracket_w = ipad_w + 0.5
bracket_h = 1.6
bracket = Rectangle((bracket_x, bracket_y), bracket_w, bracket_h,
                    facecolor='#1F242C', edgecolor='#E5C983', linewidth=2, linestyle='--', zorder=4)
ax1.add_patch(bracket)
ax1.text(bracket_x + bracket_w/2, bracket_y + bracket_h*0.75, 'Rigid CNC Mount\nBracket',
         color='#BAA164', fontsize=9, fontweight='bold', ha='center', va='center')

# RoboSense Airy LiDAR (Pitched 90° Forward: Dome facing forward, Cable facing down)
# Base cylinder of Airy
airy_base_x = bracket_x - 0.1
airy_base_y = bracket_y + 0.4
airy_base_w = 0.75 # depth along camera axis
airy_base_h = 1.1  # diameter across
airy_base = Rectangle((airy_base_x - airy_base_w, airy_base_y), airy_base_w, airy_base_h,
                      facecolor='#21262D', edgecolor='#FCFBF8', linewidth=2, zorder=6)
ax1.add_patch(airy_base)
ax1.text(airy_base_x - airy_base_w/2, airy_base_y + airy_base_h*0.5, 'Airy Base\nHousing',
         color='#FCFBF8', fontsize=8.5, ha='center', va='center')

# Hemispherical Optical Dome (Facing FORWARD!)
dome_center_x = airy_base_x - airy_base_w
dome_center_y = airy_base_y + airy_base_h/2
dome_radius = airy_base_h / 2
dome_wedge = Wedge((dome_center_x, dome_center_y), dome_radius, 90, 270,
                   facecolor='#BAA164', edgecolor='#E5C983', alpha=0.85, linewidth=2.5, zorder=7)
ax1.add_patch(dome_wedge)

# LiDAR Dome Forward Optical Axis (+Z_lidar)
lidar_arrow = FancyArrowPatch((dome_center_x - dome_radius, dome_center_y),
                              (dome_center_x - dome_radius - 2.0, dome_center_y),
                              arrowstyle='-|>', mutation_scale=18, color='#4ADE80', linewidth=2.5, zorder=8)
ax1.add_patch(lidar_arrow)
ax1.text(dome_center_x - dome_radius - 1.1, dome_center_y + 0.25, 'LiDAR Optical Apex (+Z_lidar FORWARD)',
         color='#4ADE80', fontsize=9.5, fontweight='bold', ha='center')

# FOV Hemispherical Beams
fov_wedge = Wedge((dome_center_x, dome_center_y), 3.2, 105, 255,
                  facecolor='#4ADE80', alpha=0.08, zorder=1)
ax1.add_patch(fov_wedge)

# Cable Exiting from Base & Routing DOWN (+Y_cam Down)
cable_pts_x = [airy_base_x - airy_base_w*0.3, airy_base_x - airy_base_w*0.3, ipad_x + ipad_w/2, ipad_x + ipad_w/2]
cable_pts_y = [airy_base_y, bracket_y - 0.2, bracket_y - 0.5, ipad_y + 0.5]
ax1.plot(cable_pts_x, cable_pts_y, color='#F87171', linewidth=4.5, solid_capstyle='round', zorder=5)

# Cable Direction Arrow
cable_arrow = FancyArrowPatch((airy_base_x - airy_base_w*0.3, airy_base_y - 0.05),
                              (airy_base_x - airy_base_w*0.3, airy_base_y - 0.9),
                              arrowstyle='-|>', mutation_scale=15, color='#F87171', linewidth=2.5, zorder=6)
ax1.add_patch(cable_arrow)
ax1.text(airy_base_x - airy_base_w*0.3 - 0.15, airy_base_y - 0.5, 'CABLE FACING DOWN\n(Routing to USB-C)',
         color='#F87171', fontsize=9, fontweight='bold', ha='right', va='center')

ax1.set_xlim(-2.5, 3.8)
ax1.set_ylim(0.0, 9.5)
ax1.set_aspect('equal')
ax1.axis('off')

# Subplot 2: Front / Back Elevation View
ax2 = fig.add_axes([0.48, 0.48, 0.47, 0.42], facecolor='#161B22')
ax2.set_title('FRONT / REAR ELEVATION\nParallel Forward Alignment & Spacing',
              fontsize=12, fontweight='bold', color='#FCFBF8', pad=10)

# iPad Screen / Back
ib_w = 4.2
ib_h = 5.8
ib_x = 1.0
ib_y = 0.5
ipad_face = Rectangle((ib_x, ib_y), ib_w, ib_h, facecolor='#21262D', edgecolor='#8B949E', linewidth=2, zorder=2)
ax2.add_patch(ipad_face)

# iPad Rear Camera position (Top-Left from back)
ic_x = ib_x + 0.7
ic_y = ib_y + ib_h - 0.7
cam_circ = Circle((ic_x, ic_y), 0.32, facecolor='#1E293B', edgecolor='#BAA164', linewidth=2, zorder=4)
ax2.add_patch(cam_circ)
ax2.text(ic_x, ic_y - 0.65, 'iPad Camera\n(Wide 12MP)', color='#E5C983', fontsize=9, ha='center', fontweight='bold')

# Airy Mounted Centered / Offset on Top Clamp
ia_x = ib_x + ib_w*0.5
ia_y = ib_y + ib_h + 0.9
airy_dome_front = Circle((ia_x, ia_y), 0.75, facecolor='#BAA164', edgecolor='#E5C983', linewidth=2.5, alpha=0.9, zorder=4)
ax2.add_patch(airy_dome_front)
airy_core = Circle((ia_x, ia_y), 0.35, facecolor='#161B22', edgecolor='#FCFBF8', linewidth=1.5, zorder=5)
ax2.add_patch(airy_core)
ax2.text(ia_x, ia_y, 'AIRY DOME\nLOOKING\nFORWARD', color='#FCFBF8', fontsize=8, fontweight='bold', ha='center', va='center')

# Mount Clamp holding Airy
mclamp = Rectangle((ia_x - 0.9, ib_y + ib_h - 0.4), 1.8, 0.8,
                   facecolor='#30363D', edgecolor='#E5C983', linewidth=1.5, zorder=3)
ax2.add_patch(mclamp)

# Cable exiting downward from bottom of Airy housing
c_down = FancyArrowPatch((ia_x, ia_y - 0.75), (ia_x, ib_y + ib_h*0.3),
                         arrowstyle='-|>', mutation_scale=16, color='#F87171', linewidth=4, zorder=6)
ax2.add_patch(c_down)
ax2.text(ia_x + 0.25, ib_y + ib_h*0.6, 'Pigtail Cable Exit\nFacing Straight Down',
         color='#F87171', fontsize=9, fontweight='bold', va='center')

# USB-C Port at bottom
usbc_port = Rectangle((ib_x + ib_w/2 - 0.35, ib_y - 0.15), 0.7, 0.3, facecolor='#BAA164', edgecolor='#FCFBF8', zorder=4)
ax2.add_patch(usbc_port)
ax2.text(ib_x + ib_w/2, ib_y - 0.45, 'USB-C Port (Base)', color='#BAA164', fontsize=9, ha='center', fontweight='bold')

ax2.set_xlim(0.0, 6.2)
ax2.set_ylim(-0.8, 8.2)
ax2.set_aspect('equal')
ax2.axis('off')

# Subplot 3: Coordinate Frames & Matrix Confirmation Card
ax3 = fig.add_axes([0.48, 0.08, 0.47, 0.35], facecolor='#161B22')
ax3.axis('off')

card_text = """
MATHEMATICAL RIG EXTRINSIC CONFIRMATION (90.0° Pitch Forward-Facing):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
• Mount Pitch:       90.0° Forward Pitch (100% Zero 45° Tilt)
• Dome Orientation:  +Z_lidar points directly along +Z_cam (FORWARD Line-of-Sight)
• Cable Connector:   -X_lidar points directly along +Y_cam (STRAIGHT DOWN)
• Horizontal Axis:   +Y_lidar points directly along +X_cam (RIGHT)

Nominal Rotation Matrix (LiDAR Base Frame ➔ Camera Optical Frame):
  R_cam_lidar = ⎡  0.0   1.0   0.0 ⎤    det(R) = +1.000 (Pure SO(3) Rotation)
                ⎢ -1.0   0.0   0.0 ⎥    • Dome Vector (0,0,1)_L ➔ (0,0,1)_C (FORWARD)
                ⎣  0.0   0.0   1.0 ⎦    • Cable Vector (-1,0,0)_L ➔ (0,1,0)_C (DOWN)

iPad 10th Gen (A2696) Translation Vector:
  T = [+43.4 mm Right,  -63.1 mm Up (Above Camera),  -30.0 mm Forward]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✓ Compatible with FAST-LIVO2, Scan Context, AV Showcase, & AV Structure
"""
ax3.text(0.03, 0.5, card_text, color='#FCFBF8', fontfamily='monospace', fontsize=9.2, va='center')

plt.savefig('docs/assets/mount_orientation_schematic.png', dpi=200, bbox_inches='tight', facecolor=fig.get_facecolor())
print("Saved docs/assets/mount_orientation_schematic.png successfully")
