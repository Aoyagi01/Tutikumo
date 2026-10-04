#include "camera/game_camera.hpp"

#include "camera/camera_input.hpp"
#include "camera/lock_on.hpp"
#include "platform/utf8_path.hpp"
#include "settings/settings.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace tutikumo::camera {

namespace {
// The mouse and a drag on a touch screen move the camera the same way,
// aiming included: a pointer's motion rather than a held rate.
Turn combine(const Turn &a, const Turn &b) {
    return {a.yaw_degrees + b.yaw_degrees, a.pitch_degrees + b.pitch_degrees, a.yaw_held || b.yaw_held,
            a.pitch_held || b.pitch_held};
}
Turn take_pointer() {
    const Turn mouse = take(Source::Mouse);
    return combine(mouse, take(Source::Touch));
}
Turn peek_pointer() { return combine(peek(Source::Mouse), peek(Source::Touch)); }
} // namespace
namespace {

// NPJB-40001: the ordinary camera update calls the rotation helper at
// 0x088E6264. At that call s1 is the camera, a1 points to the rotation angles,
// and sp+0x30 holds the eye offset from the look-at point. The game's
// subsequent transform, terrain and wall collision checks still run on the
// adjusted offset. These facts come from this project's own reading of the
// executable; each one the driver relies on is listed in kSignature.
constexpr std::uint32_t kRotationHelper = 0x08878B70u;
constexpr std::uint32_t kCameraReturn = 0x088E626Cu;
constexpr float kRadians = 3.14159265358979323846f / 180.0f;
constexpr float kAngleUnits = 65536.0f / 360.0f;

// Camera structure, relative to s1.
constexpr std::uint32_t kEyeY = 0x04u;
constexpr std::uint32_t kPreset = 0x70u;
constexpr std::uint32_t kMode = 0x76u;
constexpr std::uint32_t kYawTarget = 0x80u;
constexpr std::uint32_t kYawCurrent = 0x82u;
constexpr std::uint32_t kButtons = 0x84u;
constexpr std::uint32_t kSnap = 0x8Eu;
// The aim the weapon reports while the player aims a bow or a bowgun, -1
// otherwise. The camera update asks the weapon's code each update (0x088E5434)
// and, while this is not negative, turns the camera after the aim itself
// (0x088E7AFC). The aim is moved with the same stick.
constexpr std::uint32_t kAim = 0x91u;
// While a weapon aims, the aim itself lives in the hunter the camera follows
// (s5 at the ordinary call). The weapon's aim code (in game_task, around
// 0x0A0FE4D8) reads the stick as on/off commands and, in the states where
// aiming may move, steps the facing by a fixed 512 or 624 and one of three
// vertical aims by a fixed amount, which is why its aim feels like a D-pad.
// The driver leaves every decision to that code -- whether the aim may move
// now, and which way -- and only replaces how far each step goes.
//
// The facing, in the camera's yaw units, the game keeps at kHunterHeading
// and copies into kHunterYaw each frame; writing only the copy is undone.
constexpr std::uint32_t kHunterYaw = 0x74u;
constexpr std::uint32_t kHunterHeading = 0x188u;
// The largest single step the aim code makes, with room for its diagonal
// scaling; anything larger is the game setting the facing for another reason.
constexpr int kLargestYawStep = 1024;

// The three vertical aims the aim code steps, depending on how the weapon
// aims. The byte ones step by 5 or 8 and stop at 100; the halfword one steps
// by that times 64 and stops at 8192. One byte unit is taken as 3.43 / 8 of
// a degree, the angle of the game's yaw step for its vertical step.
struct PitchField {
    std::uint32_t offset;
    bool halfword;
    int limit;
    int largest_step;
    float units_per_degree;
};
constexpr float kAimPitchUnitsPerDegree = 8.0f / (624.0f / kAngleUnits);
constexpr std::array<PitchField, 3> kPitchFields{{
    {0xC22u, false, 100, 16, kAimPitchUnitsPerDegree},           // 0x088A68A8
    {0x1457u, false, 100, 16, kAimPitchUnitsPerDegree},          // 0x0A11FCF0
    {0xC24u, true, 8192, 1024, kAimPitchUnitsPerDegree * 64.0f}, // 0x0A11FC9C
}};
constexpr std::uint32_t kHunterExtent = 0x1458u;
// A bowgun's scope (the full-screen reticle, entered with a short press of
// R): bit 0x1000 of the hunter's word at +0xBB0, which the camera update
// tests at 0x088E4DCC before it builds the scope's view. The camera mode stays
// 0 and the weapon reports no aim at +0x91, so without this the scope looks
// like the ordinary follow camera. The scope's aim code steps the same facing
// (by 400) and +0xC22 (by 4) from either stick, so it is driven like an aim.
constexpr std::uint32_t kHunterWeaponFlags = 0xBB0u;
constexpr std::uint32_t kScopeFlag = 0x1000u;
// Preset, relative to its pointer, and the update's stack frame.
constexpr std::uint32_t kPresetTargetHeight = 0x10u;
constexpr std::uint32_t kStackEyeY = 0x34u;
constexpr std::uint32_t kStackEyeZ = 0x38u;

// The mouse while aiming. Its degrees wait for a step of the game to size
// (the game may step an update after the stick showed the direction); up to
// this much, and for this many updates without a step, after which they are
// dropped, as a push of the stick is while the game makes no step.
constexpr float kMouseAimCarry = 45.0f;
constexpr unsigned kMouseAimWaits = 3u;
// Less than this, on an axis, is not presented to the game as a push on it.
constexpr float kMouseAimThreshold = 0.05f;
// Degrees of mouse yaw in one frame that switch on the game's own turn.
constexpr float kMouseStockTurn = 0.5f;

// The one camera mode with a driver: the ordinary follow camera in a quest.
// Others (aiming with a bow or a bowgun among them) keep the stock camera and
// the stock stick until each is traced and given its own driver below.
constexpr std::uint8_t kFollowMode = 0u;

// Game code the driver depends on, read from NPJB-40001. If any word differs,
// the executable is not the one these facts were read from, and the driver
// stays out rather than write to places that may mean something else.
constexpr std::array<CodeWord, 24> kSignature{{
    {0x088E6264u, 0x0E21E2DCu, "jal 0x08878B70: the camera update calls the rotation helper"},
    {0x088E6254u, 0x27A50050u, "addiu a1,sp,0x50: the helper's angles are on the update's stack"},
    {0x08878B70u, 0x27BDFFE0u, "addiu sp,sp,-0x20: the rotation helper's first instruction"},
    {0x088E6214u, 0x92230076u, "lbu v1,0x76(s1): the camera mode"},
    {0x088E68F0u, 0x8E220070u, "lw v0,0x70(s1): the camera's preset"},
    {0x088E68F8u, 0xE7A00034u, "swc1 f0,0x34(sp): the eye offset's height, from the preset"},
    {0x088E6904u, 0xE7A20038u, "swc1 f2,0x38(sp): the eye offset's distance, from the preset"},
    {0x088E6908u, 0xC4410010u, "lwc1 f1,0x10(v0): the preset's look-at height"},
    {0x088E6910u, 0x0A23988Fu, "j 0x088E623C: modes 0-2 join the path that calls the helper"},
    {0x088E61A0u, 0xA6220080u, "sh v0,0x80(s1): the target yaw"},
    {0x088E61A4u, 0x86220082u, "lh v0,0x82(s1): the filtered yaw"},
    {0x088E61ACu, 0x9224008Eu, "lbu a0,0x8e(s1): the recentre snap"},
    {0x088E77D4u, 0x96260084u, "lhu a2,0x84(s1): the camera's buttons"},
    {0x088E6B58u, 0xC6220004u, "lwc1 f2,0x4(s1): the eye height the 1/8 filter eases"},
    {0x0896D47Cu, 0x3E000000u, "0.125: that filter's coefficient"},
    {0x088E5434u, 0x0E83D878u, "jal 0x0A0F61E0: the camera asks the weapon whether it is aiming"},
    {0x088E5448u, 0x82220091u, "lb v0,0x91(s1): the aim the weapon reported"},
    {0x088E7AFCu, 0x82260091u, "lb a2,0x91(s1): the camera follows the aim while it is not negative"},
    {0x088E7B20u, 0x8EA20074u, "lw v0,0x74(s5): what it follows is the hunter's facing"},
    {0x088E34C8u, 0x82630C22u, "lb v1,0xc22(s3): and the hunter's vertical aim"},
    {0x088A68A8u, 0x90820C22u, "lbu v0,0xc22(a0): the game's own step of the vertical aim"},
    {0x088A68C8u, 0x24020064u, "addiu v0,zero,0x64: which it limits to 100"},
    {0x088E4DCCu, 0x8C420BB0u, "lw v0,0xbb0(v0): the camera reads the hunter's weapon flags"},
    {0x088E4DD0u, 0x30421000u, "andi v0,v0,0x1000: and builds the scope's view while this bit is set"},
}};

struct State {
    bool prepared{};
    bool hooked{};
    RotationFunction original{};
    std::uint64_t frame{};
    std::uint64_t last_update{};
    std::uint32_t address{};
    bool available{};
    float yaw_remainder{};
    bool pitch_owned{};
    // The pitch above is lock-on's (lock_on.hpp), not the player's.
    bool pitch_by_lock{};
    float pitch{};
    unsigned updates{};
    bool aiming{};
    std::uint32_t aim_hunter{};
    std::uint16_t aim_heading{};
    std::array<int, 3> aim_pitch{};
    float aim_yaw_remainder{};
    float aim_pitch_remainder{};
    // The mouse's aim not yet spent on a step, in degrees, and the frame
    // after the one its direction was last shown to the game (0: never).
    float mouse_yaw{};
    float mouse_pitch{};
    unsigned mouse_yaw_waits{};
    unsigned mouse_pitch_waits{};
    std::uint64_t mouse_shown{};
    // The direction last shown, each axis -1, 0 or +1.
    int shown_x{};
    int shown_y{};
    // The game's own steps for a shown direction, learned from the steps it
    // made in this aim: size (straight and diagonal) and which way it went
    // for a positive push (+1, -1; 0: not seen yet). Scope and aim step by
    // different amounts, so a change between them starts over.
    bool learned_scoped{};
    std::array<int, 2> yaw_step{};
    int yaw_sign{};
    std::array<int, 2> pitch_step{};
    int pitch_sign{};
    std::size_t pitch_field{};
    // The game's next step, taken off the aim in advance at the flip (see
    // game_camera_anticipate_aim), and the values that left in the hunter.
    int anticipated_yaw{};
    std::uint16_t anticipated_heading{};
    int anticipated_pitch{};
    int anticipated_pitch_value{};
};
State state;

// MHP2G (ULJM05500): the camera update at 0x08886B54. Its argument is the
// camera's owner; the camera itself, with the same yaw fields as MHP3rd's
// (+0x80 target, +0x82 shown), is 0xA0 into it. While the D-pad's left or
// right is held the update adds or subtracts 1150 to the target
// (0x08887174..0x088871D8); then it eases the shown yaw a quarter of the way
// to the target (0x08887318..0x08887334). An update for a frame where the
// game's scene flag at +0xA8D of the object at 0x089C6CB4 is set returns
// early without touching the camera (0x08886B84..0x08886BA4); so does this
// driver. Every fact used is listed in kMhp2gSignature.
constexpr std::uint32_t kMhp2gCameraUpdate = 0x08886B54u;
constexpr std::uint32_t kMhp2gCameraOffset = 0xA0u;
constexpr std::uint32_t kMhp2gScenePointer = 0x089C6CB4u;
constexpr std::uint32_t kMhp2gSceneFlag = 0xA8Du;
// MHP2G has no vertical camera control. The update takes the eye's offset
// from the look-at point, (0, height, distance) before it is turned by the
// yaw, from the camera's preset (+0x70): height at +0x4, distance at +0x8
// (0x088874E8, 0x08887518..0x08887524). For a pitch the driver hands the
// update a copy of the preset with that offset turned about the look-at
// point, keeping its length, and puts the game's own preset back after. The
// copy lives in kernel memory the game never uses.
constexpr std::uint32_t kMhp2gPresetCopy = 0x08110000u;
constexpr std::uint32_t kMhp2gPresetBytes = 0x20u;
constexpr std::uint32_t kMhp2gPresetHeight = 0x04u;
constexpr std::uint32_t kMhp2gPresetDistance = 0x08u;
constexpr std::uint32_t kMhp2gPresetLookAt = 0x10u;  // the look-at point's height above the hunter
// How far up the eye goes, in degrees, while a wall pulls it to the hunter.
constexpr float kMhp2gWallRise = 25.0f;
// How far the eye may orbit, in degrees above the ground's horizontal.
constexpr float kMhp2gLowestElevation = -15.0f;
constexpr float kMhp2gHighestElevation = 80.0f;
constexpr std::array<CodeWord, 22> kMhp2gSignature{{
    {0x08886B54u, 0x27BDFE30u, "addiu sp,sp,-0x1d0: the camera update's first instruction"},
    {0x08886B84u, 0x3C03089Cu, "lui v1,0x089c: the scene object's pointer"},
    {0x08886B88u, 0x8C706CB4u, "lw s0,0x6cb4(v1): at 0x089C6CB4"},
    {0x08886B8Cu, 0x92030A8Du, "lbu v1,0xa8d(s0): whose flag makes the update return early"},
    {0x08886B94u, 0x0080A821u, "addu s5,a0,zero: the camera's owner is the argument"},
    {0x08886BCCu, 0x26B400A0u, "addiu s4,s5,0xa0: and the camera 0xa0 into it"},
    {0x08887174u, 0x96850084u, "lhu a1,0x84(s4): the camera's buttons"},
    {0x08887178u, 0x30A200A0u, "andi v0,a1,0xa0: the D-pad's left and right turn it"},
    {0x088871A4u, 0x2403047Eu, "addiu v1,zero,1150: by this step each update"},
    {0x088871BCu, 0xA6820080u, "sh v0,0x80(s4): on the target yaw"},
    {0x0888731Cu, 0x86830082u, "lh v1,0x82(s4): the shown yaw"},
    {0x08887334u, 0xA6820082u, "sh v0,0x82(s4): eased towards the target"},
    {0x088874DCu, 0x0E21972Cu, "jal 0x08865cb0: the last hunter state check before the preset is read"},
    {0x088874E0u, 0x2406003Au, "addiu a2,zero,58: (its argument)"},
    {0x088874E4u, 0x5040000Cu, "beql v0,zero,0x08887518: a zero result reads the eye's offset from the preset"},
    {0x08865CB0u, 0x90830298u, "lbu v1,0x298(a0): the state check's first instruction"},
    {0x088874E8u, 0x8E820070u, "lw v0,0x70(s4): the camera's preset"},
    {0x08887518u, 0xC4400004u, "lwc1 f0,0x4(v0): the eye's height above the look-at point, from the preset"},
    {0x0888751Cu, 0xE7A00154u, "swc1 f0,0x154(sp): into the offset the yaw turns"},
    {0x08887520u, 0xC4400008u, "lwc1 f0,0x8(v0): the eye's distance, from the preset"},
    {0x08887524u, 0xE7A00158u, "swc1 f0,0x158(sp): into the same offset"},
    {0x08887528u, 0xC4410010u, "lwc1 f1,0x10(v0): the look-at point's height, from the preset"},
}};

// Set while the camera swings behind the hunter by itself (after a hit,
// 0x088872E0..0x0888730C), an eighth of the way each update.
constexpr std::uint32_t kMhp2gFollowFlag = 0x89u;
// Non-zero for a while after a hit: the wide view (0x08886DB8).
constexpr std::uint32_t kMhp2gHitTimer = 0x8Bu;
constexpr std::uint32_t kMhp2gFieldOfView = 0x64u;
constexpr std::uint32_t kMhp2gStateCheck = 0x08865CB0u;
constexpr std::uint32_t kMhp2gPresetRead = 0x088874E4u;  // return from the last check

struct Mhp2gState {
    bool prepared{};
    RotationFunction original{};
    RotationFunction state_check{};
    float yaw_remainder{};
    // Degrees the player has tilted the eye away from the preset's angle.
    float pitch{};
    // During one camera update: the camera whose eye is to be turned, and the
    // game's preset the copy stands in for once it has.
    std::uint32_t armed_camera{};
    std::uint32_t game_preset{};
    std::uint32_t preset_camera{};
    bool was_swinging{};
    // The eye's height above the look-at point at the last update; the rise a
    // wall gives it (degrees, eased); and, after a turn, the height the game's
    // own preset puts it at, which the turn keeps holding the distance for
    // until the eye has settled there.
    float eye_dy{};
    float rise{};
    float level_dy{};
    bool turned{};
    // For TUTIKUMO_TRACE_CAMERA: the camera of the last update, and whether
    // the stick was held in it.
    std::uint32_t last_camera{};
    bool last_held{};
    float last_yaw{};
    float last_pitch_input{};
};
Mhp2gState mhp2g;

// Gives the camera its own preset back after an update ran with the copy,
// unless the game has chosen another one since.
void mhp2g_restore_preset(psprecomp::GuestMemory &memory) {
    if (mhp2g.game_preset != 0u && memory.load32(mhp2g.preset_camera + 0x70u) == kMhp2gPresetCopy)
        memory.store32(mhp2g.preset_camera + 0x70u, mhp2g.game_preset);
    mhp2g.game_preset = 0u;
}

bool option_on() {
    const auto &s = settings::current();
    // Right stick describes a physical stick. On Android a finger drag turns
    // the camera whatever it says, so there Analog camera alone decides.
    return s.analog_camera && (s.right_stick == settings::RightStick::Camera ||
                               settings::kPlatform == settings::Platform::Android);
}

bool driving_allowed() { return state.hooked && option_on(); }

bool lock_on_allowed() { return settings::current().lock_on; }

// The game's camera commands in the camera's buttons (0x088E77D4), the
// D-pad's turn and tilt, as the PSP's buttons.
constexpr std::uint16_t kDpadCommands = 0x00F0u;

float load_float(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return std::bit_cast<float>(memory.load32(address));
}

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    memory.store32(address, std::bit_cast<std::uint32_t>(value));
}

void release() {
    state.available = false;
    state.pitch_owned = false;
    state.yaw_remainder = 0.0f;
    state.aiming = false;
    state.aim_yaw_remainder = 0.0f;
    state.aim_pitch_remainder = 0.0f;
    state.mouse_yaw = 0.0f;
    state.mouse_pitch = 0.0f;
    state.mouse_yaw_waits = 0u;
    state.mouse_pitch_waits = 0u;
}

void trace(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stack, const Turn &turn,
           bool active, bool recentre, bool vertical_command, float y, float z, float target_height) {
    static const std::filesystem::path path = environment_path("TUTIKUMO_TRACE_CAMERA_STATE");
    if (path.empty()) return;
    static std::ofstream out(path);
    const float eye_dx = load_float(memory, address) - load_float(memory, address + 0x10u);
    const float eye_dy = load_float(memory, address + 4u) - load_float(memory, address + 0x14u);
    const float eye_dz = load_float(memory, address + 8u) - load_float(memory, address + 0x18u);
    out << state.frame << ',' << std::hex << address << std::dec << ',' << active << ',' << turn.yaw_degrees << ','
        << turn.pitch_degrees << ',' << memory.load16(address + kYawTarget) << ','
        << memory.load16(address + kYawCurrent) << ',' << y << ',' << z << ',' << target_height << ','
        << state.pitch_owned << ',' << state.pitch << ',' << recentre << ',' << vertical_command << ','
        << load_float(memory, stack + kStackEyeY) << ',' << load_float(memory, stack + kStackEyeZ) << ','
        << std::atan2(eye_dy, std::hypot(eye_dx, eye_dz)) / kRadians << '\n';
    if (state.updates % 30u == 0u) out.flush();
}

// The ordinary follow camera: yaw on the game's own angle, pitch by orbiting
// the eye around its look-at point at the preset's distance.
void drive_follow(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx, std::uint32_t address,
                  std::uint32_t stack) {
    if (state.address != address || state.frame - state.last_update > 2u) {
        state.pitch_owned = false;
        state.yaw_remainder = 0.0f;
    }
    state.address = address;
    state.last_update = state.frame;
    state.available = true;
    state.aiming = false;
    ++state.updates;

    const bool active = driving_allowed();
    // Taken even when inactive, so nothing built up while the option was off
    // is spent the moment it comes back on.
    const Turn turn = take();
    const float y = load_float(memory, stack + kStackEyeY);
    const float z = load_float(memory, stack + kStackEyeZ);
    const auto preset = memory.load32(address + kPreset);
    if (!memory.raw_pointer(preset, 0x20u)) return;
    const float target_height = load_float(memory, preset + kPresetTargetHeight);
    const float radius = std::hypot(y - target_height, z);
    if (!std::isfinite(radius) || radius < 1.0f || radius > 10000.0f) return;

    // A real D-pad command, recentre or target-camera snap cancels our pitch.
    // The game has already selected the new preset; these stay its controls.
    const auto buttons = memory.load16(address + kButtons);
    const bool recentre = (buttons & 0x100u) != 0u || memory.load8(address + kSnap) != 0u;
    const bool vertical_command = (buttons & 0x50u) != 0u;

    // Lock-on (lock_on.hpp) aims the same angles while it holds a monster.
    std::optional<LockOnAim> lock;
    if (lock_on_allowed() && lock_on_wanted()) {
        LockOnCamera camera;
        camera.address = address;
        camera.yaw = memory.load16(address + kYawCurrent);
        camera.look_at = {load_float(memory, address + 0x10u), load_float(memory, address + 0x14u),
                          load_float(memory, address + 0x18u)};
        camera.manual_yaw = turn.yaw_held ? turn.yaw_degrees : 0.0f;
        // Recentring (L) shows as the snap; holding L for the item bar
        // does not. The D-pad turns and tilts.
        camera.command = memory.load8(address + kSnap) != 0u || (buttons & kDpadCommands) != 0u;
        camera.pitch = std::atan2(y - target_height, std::fabs(z)) / kRadians;
        if (state.pitch_owned) camera.pitch = state.pitch;
        camera.player_pitch = (active && turn.pitch_held) || (state.pitch_owned && !state.pitch_by_lock);
        lock = lock_on_update(memory, camera);
    }
    const bool lock_pitch = lock && lock->pitch;
    if ((!active && !lock_pitch) || recentre || vertical_command) state.pitch_owned = false;

    if (lock) {
        state.yaw_remainder = 0.0f;
        memory.store16(address + kYawTarget, lock->yaw);
        memory.store16(address + kYawCurrent, lock->yaw);
        memory.store32(ctx.gpr[5] + 4u, static_cast<std::uint32_t>(static_cast<std::int16_t>(lock->yaw)));
    } else if (active && !recentre && turn.yaw_held) {
        state.yaw_remainder -= turn.yaw_degrees * kAngleUnits;
        const int step = static_cast<int>(state.yaw_remainder);
        state.yaw_remainder -= static_cast<float>(step);
        const auto yaw = static_cast<std::uint16_t>(memory.load16(address + kYawCurrent) + step);
        // Keeping target and filtered yaw together prevents a delayed turn
        // after release: the filter at 0x088E61A4 has nothing left to close.
        memory.store16(address + kYawTarget, yaw);
        memory.store16(address + kYawCurrent, yaw);
        memory.store32(ctx.gpr[5] + 4u, static_cast<std::uint32_t>(static_cast<std::int16_t>(yaw)));
    } else {
        state.yaw_remainder = 0.0f;
    }

    if (lock_pitch || (active && !recentre && !vertical_command)) {
        float previous_pitch = state.pitch;
        if (lock_pitch) {
            if (!state.pitch_owned) previous_pitch = std::atan2(y - target_height, std::fabs(z)) / kRadians;
            state.pitch = *lock->pitch;
            state.pitch_owned = true;
            state.pitch_by_lock = true;
        } else if (turn.pitch_held) {
            if (!state.pitch_owned) {
                state.pitch = std::atan2(y - target_height, std::fabs(z)) / kRadians;
                previous_pitch = state.pitch;
                state.pitch_owned = true;
            }
            state.pitch_by_lock = false;
            state.pitch = std::clamp(state.pitch + turn.pitch_degrees, -60.0f, 70.0f);
        }
        if (state.pitch_owned) {
            // Rotate the eye about its look-at point, keeping the current
            // preset's distance. Only this update's stack values change; shared
            // preset tables and other cameras are never written.
            const float pitch = state.pitch * kRadians;
            store_float(memory, stack + kStackEyeY, target_height + radius * std::sin(pitch));
            store_float(memory, stack + kStackEyeZ, std::copysign(radius * std::cos(pitch), z));
            // 0x088E6B58 eases eye.y towards target.y by 1/8 each update.
            // Advance its current value by just the player's height change;
            // otherwise releasing the stick leaves many frames of catch-up.
            // Terrain movement and collision corrections still use the game's
            // filter, and all subsequent collision checks remain in place.
            if (state.pitch != previous_pitch) {
                const float delta = radius * (std::sin(pitch) - std::sin(previous_pitch * kRadians));
                store_float(memory, address + kEyeY, load_float(memory, address + kEyeY) + delta);
            }
        }
    }
    trace(memory, address, stack, turn, active, recentre, vertical_command, y, z, target_height);
}

// TUTIKUMO_TRACE_CAMERA_MODES=path.csv: every call of the rotation helper, from
// any caller, with the camera mode and the stick, and the whole camera
// structure whenever the mode is not the ordinary one. For finding what the
// aiming camera keeps where, before it has a driver.
std::ofstream *modes_trace() {
    static const std::filesystem::path path = environment_path("TUTIKUMO_TRACE_CAMERA_MODES");
    if (path.empty()) return nullptr;
    static std::ofstream out(path);
    return &out;
}

void dump_camera(std::ostream &out, const psprecomp::GuestMemory &memory, std::uint32_t address) {
    const unsigned mode = memory.load8(address + kMode);
    out << ",mode=" << mode << ",yaw=" << memory.load16(address + kYawTarget) << ':'
        << memory.load16(address + kYawCurrent) << ",buttons=" << std::hex << memory.load16(address + kButtons)
        << std::dec;
    out << ",words=" << std::hex;
    for (std::uint32_t offset = 0u; offset < 0x190u; offset += 4u)
        out << (offset ? ":" : "") << memory.load32(address + offset);
    out << std::dec;
}

// Once a flip as well, from the last camera the ordinary update named: an
// aiming camera may never call the helper at all.
void trace_flip(const psprecomp::GuestMemory &memory) {
    std::ofstream *out = modes_trace();
    if (out == nullptr || state.address == 0u || memory.raw_pointer(state.address, 0x190u) == nullptr) return;
    const Rate stick = rate(Source::Stick);
    *out << state.frame << ",flip,s1=" << std::hex << state.address << std::dec << ",stick=" << stick.yaw << ':'
         << stick.pitch << ",aim=" << static_cast<int>(static_cast<std::int8_t>(memory.load8(state.address + kAim)));
    // The flag 0x088E53E8 tests before asking the weapon at all.
    const std::uint32_t game = memory.load32(0x08AB3640u);
    if (memory.raw_pointer(game + 0x60474u, 4u) != nullptr)
        *out << ",aimflag=" << std::hex << memory.load32(game + 0x60474u) << std::dec;
    dump_camera(*out, memory, state.address);
    *out << std::endl;
}

void trace_modes(const psprecomp::GuestMemory &memory, const psprecomp::AllegrexContext &ctx) {
    std::ofstream *trace_out = modes_trace();
    if (trace_out == nullptr) return;
    std::ofstream &out = *trace_out;
    // Only the camera's own calls; the helper also turns every other object.
    if (ctx.gpr[31] != kCameraReturn && ctx.gpr[31] != 0x088E5E24u) return;
    const auto address = ctx.gpr[17];
    const bool camera = memory.raw_pointer(address, 0x190u) != nullptr;
    const Rate stick = rate(Source::Stick);
    out << state.frame << ",ra=" << std::hex << ctx.gpr[31] << ",s1=" << address << std::dec;
    if (memory.raw_pointer(ctx.gpr[5], 12u) != nullptr)
        out << ",angles=" << static_cast<std::int32_t>(memory.load32(ctx.gpr[5])) << ':'
            << static_cast<std::int32_t>(memory.load32(ctx.gpr[5] + 4u)) << ':'
            << static_cast<std::int32_t>(memory.load32(ctx.gpr[5] + 8u));
    out << ",stick=" << stick.yaw << ':' << stick.pitch;
    if (camera) dump_camera(out, memory, address);
    // While the weapon aims, the object the camera follows (s5 at the
    // ordinary call), which carries the aim the camera turns after.
    const auto followed = ctx.gpr[21];
    if (camera && ctx.gpr[31] == kCameraReturn &&
        static_cast<std::int8_t>(memory.load8(address + kAim)) >= 0 &&
        memory.raw_pointer(followed, 0x2000u) != nullptr) {
        out << ",s5=" << std::hex << followed << ",followed=";
        for (std::uint32_t offset = 0u; offset < 0x2000u; offset += 4u)
            out << (offset ? ":" : "") << memory.load32(followed + offset);
        out << std::dec;
    }
    out << '\n';
    out.flush();
}

bool scoped(const psprecomp::GuestMemory &memory, std::uint32_t hunter) {
    return memory.raw_pointer(hunter, kHunterExtent) != nullptr &&
           (memory.load32(hunter + kHunterWeaponFlags) & kScopeFlag) != 0u;
}

// TUTIKUMO_TRACE_AIM=path.csv: one line per aiming camera update with what the
// game stepped and what the driver made of it, and one per direction the
// mouse showed the game. For telling the game's steps from the driver's.
std::ofstream *aim_trace() {
    static const std::filesystem::path path = environment_path("TUTIKUMO_TRACE_AIM");
    if (path.empty()) return nullptr;
    static std::ofstream out(path);
    return &out;
}

int load_pitch(const psprecomp::GuestMemory &memory, std::uint32_t hunter, const PitchField &field) {
    return field.halfword ? static_cast<std::int16_t>(memory.load16(hunter + field.offset))
                          : static_cast<std::int8_t>(memory.load8(hunter + field.offset));
}

void store_pitch(psprecomp::GuestMemory &memory, std::uint32_t hunter, const PitchField &field, int value) {
    if (field.halfword)
        memory.store16(hunter + field.offset, static_cast<std::uint16_t>(static_cast<std::int16_t>(value)));
    else
        memory.store8(hunter + field.offset, static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
}

void remember_aim(const psprecomp::GuestMemory &memory, std::uint32_t hunter) {
    state.aim_hunter = hunter;
    state.aim_heading = memory.load16(hunter + kHunterHeading);
    for (std::size_t i = 0; i < kPitchFields.size(); ++i) state.aim_pitch[i] = load_pitch(memory, hunter, kPitchFields[i]);
}

void store_heading(psprecomp::GuestMemory &memory, std::uint32_t hunter, std::uint16_t heading) {
    memory.store16(hunter + kHunterHeading, heading);
    memory.store16(hunter + kHunterYaw, heading);
}

int sign_of(float value) { return value > 0.0f ? 1 : (value < 0.0f ? -1 : 0); }

// The direction the mouse shows the game this frame, each axis -1, 0 or +1:
// every axis with degrees waiting is pushed all the way, so the game steps it
// and the driver sizes the step. A push the game sees as a clear on or off per
// axis is also one whose step can be told in advance.
std::optional<std::pair<int, int>> mouse_direction() {
    if (!game_camera_aim_boost()) return std::nullopt;
    const Turn pending = peek_pointer();
    const float yaw = state.mouse_yaw + pending.yaw_degrees;
    const float pitch = state.mouse_pitch + pending.pitch_degrees;
    const int x = std::fabs(yaw) >= kMouseAimThreshold ? sign_of(yaw) : 0;
    const int y = std::fabs(pitch) >= kMouseAimThreshold ? sign_of(pitch) : 0;
    if (x == 0 && y == 0) return std::nullopt;
    return std::pair{x, y};
}

// Puts back what game_camera_anticipate_aim took off, if it is still there:
// the game has since added its own step to it, or has not run its aim code at
// all. A value the game has set outright (a roll, the aim ending) is left be.
void settle_anticipation(psprecomp::GuestMemory &memory) {
    const std::uint32_t hunter = state.aim_hunter;
    const bool valid = memory.raw_pointer(hunter, kHunterExtent) != nullptr;
    if (valid && state.anticipated_yaw != 0) {
        const auto heading = memory.load16(hunter + kHunterHeading);
        const int game = static_cast<std::int16_t>(static_cast<std::uint16_t>(heading - state.anticipated_heading));
        if (std::abs(game) <= kLargestYawStep)
            store_heading(memory, hunter, static_cast<std::uint16_t>(heading + state.anticipated_yaw));
    }
    if (valid && state.anticipated_pitch != 0) {
        const PitchField &field = kPitchFields[state.pitch_field];
        const int value = load_pitch(memory, hunter, field);
        if (std::abs(value - state.anticipated_pitch_value) <= field.largest_step)
            store_pitch(memory, hunter, field,
                        std::clamp(value + state.anticipated_pitch, -field.limit, field.limit));
    }
    state.anticipated_yaw = 0;
    state.anticipated_pitch = 0;
}

void forget_steps(bool scope) {
    state.learned_scoped = scope;
    state.yaw_step = {};
    state.yaw_sign = 0;
    state.pitch_step = {};
    state.pitch_sign = 0;
}

// Learns how the game steps for the direction it was shown: how far, and
// which way for a positive push.
void learn_steps(bool scope, int game_yaw, int game_pitch, std::size_t pitch_field) {
    if (scope != state.learned_scoped) forget_steps(scope);
    const std::size_t diagonal = state.shown_x != 0 && state.shown_y != 0 ? 1u : 0u;
    if (state.shown_x != 0 && game_yaw != 0) {
        state.yaw_step[diagonal] = std::abs(game_yaw);
        state.yaw_sign = (game_yaw > 0 ? 1 : -1) * state.shown_x;
    }
    if (state.shown_y != 0 && game_pitch != 0) {
        state.pitch_field = pitch_field;
        state.pitch_step[diagonal] = std::abs(game_pitch);
        state.pitch_sign = (game_pitch > 0 ? 1 : -1) * state.shown_y;
    }
}

// A bow or a bowgun aiming, or a bowgun's scope. The stick reaches the game
// stretched to full length (game_camera_aim_boost), so its aim code steps
// whenever the player pushes at all and the game allows it; here each step
// the game made since the previous update is replaced by one in proportion to
// the stick, in the game's direction. No step from the game -- rolling,
// moving, firing, a state that locks an axis -- means no movement from the
// port either. A step made while the right stick is idle (the left stick in
// the scope) is kept as is.
//
// The mouse works the same way through the direction game_camera_mouse_aim()
// shows the game: a step made while the stick is idle and the mouse's
// direction was shown is sized by the mouse's degrees instead, and none left
// means the step is taken back.
void drive_aim(psprecomp::GuestMemory &memory, const psprecomp::AllegrexContext &ctx, std::uint32_t address) {
    const auto hunter = ctx.gpr[21];
    state.address = address;
    const int anticipated_yaw = state.anticipated_yaw;
    const int anticipated_pitch = state.anticipated_pitch;
    settle_anticipation(memory);
    if (!driving_allowed() || memory.raw_pointer(hunter, kHunterExtent) == nullptr) {
        release();
        return;
    }
    // The camera and the stick are the game's while aiming.
    state.available = false;
    state.pitch_owned = false;
    state.yaw_remainder = 0.0f;
    state.last_update = state.frame;
    ++state.updates;
    const Turn mouse = take_pointer();
    const Turn turn = take();
    const bool scope = scoped(memory, hunter);
    if (!state.aiming || state.aim_hunter != hunter) {
        // The first update of an aim only learns where the aim starts.
        state.aiming = true;
        state.aim_yaw_remainder = 0.0f;
        state.aim_pitch_remainder = 0.0f;
        state.mouse_yaw = 0.0f;
        state.mouse_pitch = 0.0f;
        state.mouse_yaw_waits = 0u;
        state.mouse_pitch_waits = 0u;
        forget_steps(scope);
        remember_aim(memory, hunter);
        return;
    }
    state.mouse_yaw = std::clamp(state.mouse_yaw + mouse.yaw_degrees, -kMouseAimCarry, kMouseAimCarry);
    state.mouse_pitch = std::clamp(state.mouse_pitch + mouse.pitch_degrees, -kMouseAimCarry, kMouseAimCarry);
    // The game's step answers the mouse only if the stick was idle and the
    // mouse's direction was on the second stick this frame or the last.
    const bool mouse_shown = state.mouse_shown != 0u && state.frame + 1u - state.mouse_shown <= 1u;
    const bool shown_now = state.mouse_shown == state.frame + 1u;
    bool yaw_spent = false;
    bool pitch_spent = false;

    const float carried_yaw = state.mouse_yaw;
    const auto heading = memory.load16(hunter + kHunterHeading);
    const int game_yaw = static_cast<std::int16_t>(static_cast<std::uint16_t>(heading - state.aim_heading));
    if (game_yaw != 0 && std::abs(game_yaw) <= kLargestYawStep && (turn.yaw_held || mouse_shown)) {
        float degrees = std::fabs(turn.yaw_degrees);
        if (!turn.yaw_held) {
            degrees = std::fabs(state.mouse_yaw);
            state.mouse_yaw = 0.0f;
            yaw_spent = true;
        }
        state.aim_yaw_remainder += degrees * kAngleUnits;
        const int step = static_cast<int>(state.aim_yaw_remainder);
        state.aim_yaw_remainder -= static_cast<float>(step);
        store_heading(memory, hunter, static_cast<std::uint16_t>(state.aim_heading + (game_yaw > 0 ? step : -step)));
    } else if (game_yaw == 0) {
        state.aim_yaw_remainder = 0.0f;
    }

    int game_pitch_seen = 0;
    std::size_t pitch_field_seen = 0u;
    bool pitch_stepped = false;
    for (std::size_t i = 0; i < kPitchFields.size(); ++i) {
        const PitchField &field = kPitchFields[i];
        const int current = load_pitch(memory, hunter, field);
        const int game_pitch = current - state.aim_pitch[i];
        if (game_pitch == 0 || std::abs(game_pitch) > field.largest_step) continue;
        if (game_pitch_seen == 0) {
            game_pitch_seen = game_pitch;
            pitch_field_seen = i;
        }
        if (!(turn.pitch_held || mouse_shown)) continue;
        pitch_stepped = true;
        float degrees = std::fabs(turn.pitch_degrees);
        if (!turn.pitch_held) {
            degrees = std::fabs(state.mouse_pitch);
            pitch_spent = true;
        }
        const float wanted = state.aim_pitch_remainder + degrees;
        const int step = static_cast<int>(wanted * field.units_per_degree);
        state.aim_pitch_remainder = wanted - static_cast<float>(step) / field.units_per_degree;
        const int next = std::clamp(state.aim_pitch[i] + (game_pitch > 0 ? step : -step), -field.limit, field.limit);
        if (next == field.limit || next == -field.limit) state.aim_pitch_remainder = 0.0f;
        store_pitch(memory, hunter, field, next);
    }
    if (!pitch_stepped) state.aim_pitch_remainder = 0.0f;
    // Only a step that answered this frame's direction says how the game
    // answers a direction.
    if (shown_now && !turn.yaw_held && !turn.pitch_held)
        learn_steps(scope, std::abs(game_yaw) <= kLargestYawStep ? game_yaw : 0, game_pitch_seen, pitch_field_seen);
    if (std::ofstream *out = aim_trace()) {
        const int applied = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(memory.load16(hunter + kHunterHeading) - state.aim_heading));
        *out << state.frame << ",update,scope=" << scope << ",stick=" << turn.yaw_degrees << ':'
             << turn.pitch_degrees << ",mouse=" << mouse.yaw_degrees << ':' << mouse.pitch_degrees
             << ",carried=" << carried_yaw << ",shown=" << mouse_shown << ",anticipated=" << anticipated_yaw << ':'
             << anticipated_pitch << ",game=" << game_yaw << ':' << game_pitch_seen << ",yaw=" << applied
             << ",pitch=" << load_pitch(memory, hunter, kPitchFields[0]) - state.aim_pitch[0] << '\n';
    }
    if (pitch_spent) state.mouse_pitch = 0.0f;
    // A stick in use owns the aim; otherwise mouse degrees the game has not
    // stepped for in a few updates are dropped, each axis on its own, so an
    // axis the game keeps still does not save up degrees for a jump later.
    if (turn.yaw_held || turn.pitch_held) {
        state.mouse_yaw = 0.0f;
        state.mouse_pitch = 0.0f;
    }
    const auto wait = [](float &degrees, unsigned &waits, bool spent) {
        if (spent || degrees == 0.0f) {
            waits = 0u;
        } else if (++waits > kMouseAimWaits) {
            degrees = 0.0f;
            waits = 0u;
        }
    };
    wait(state.mouse_yaw, state.mouse_yaw_waits, yaw_spent);
    wait(state.mouse_pitch, state.mouse_pitch_waits, pitch_spent);
    remember_aim(memory, hunter);
}

void adjust_camera(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    trace_modes(runtime.memory(), ctx);
    if (ctx.gpr[31] != kCameraReturn) return;
    auto &memory = runtime.memory();
    const auto address = ctx.gpr[17];
    const auto stack = ctx.gpr[29];
    if (!memory.raw_pointer(address, 0x188u) || !memory.raw_pointer(stack, 0x190u) ||
        !memory.raw_pointer(ctx.gpr[5], 12u))
        return;
    switch (memory.load8(address + kMode)) {
    case kFollowMode:
        // Aiming a bow or a bowgun, or looking through a bowgun's scope: the
        // stick moves the aim and the game's camera follows it.
        if (static_cast<std::int8_t>(memory.load8(address + kAim)) >= 0 || scoped(memory, ctx.gpr[21])) {
            drive_aim(memory, ctx, address);
            return;
        }
        drive_follow(memory, ctx, address, stack);
        return;
    // A driver for the aiming camera goes here, once traced.
    default:
        release();
        return;
    }
}

// MHP2G: runs in place of the camera update, turns the camera by what the
// stick asks for, then continues the update with the same context.
void mhp2g_camera_update(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    auto &memory = runtime.memory();
    const std::uint32_t camera = ctx.gpr[4] + kMhp2gCameraOffset;
    const std::uint32_t scene = memory.load32(kMhp2gScenePointer);
    const bool scene_ok = memory.raw_pointer(scene + kMhp2gSceneFlag, 1u) != nullptr &&
                          memory.load8(scene + kMhp2gSceneFlag) == 0u;
    mhp2g_restore_preset(memory);
    mhp2g.armed_camera = 0u;
    if (scene_ok && memory.raw_pointer(camera, 0x90u) != nullptr) {
        state.available = true;
        state.last_update = state.frame;
        if (driving_allowed()) {
            const Turn turn = take();
            const bool held = turn.yaw_held || turn.pitch_held;
            mhp2g.last_camera = camera;
            mhp2g.last_held = held;
            mhp2g.last_yaw = turn.yaw_degrees;
            mhp2g.last_pitch_input = turn.pitch_degrees;
            // L brings the camera behind the hunter (+0x8E while it swings):
            // the eye goes back to the preset's angle as well, once, when the
            // swing starts. Reset on every update of the swing, a pitch could
            // not be given until it ended.
            const bool swinging = memory.load8(camera + kSnap) != 0u;
            if (swinging && !mhp2g.was_swinging && !held) mhp2g.pitch = 0.0f;
            mhp2g.was_swinging = swinging;
            // While the player turns the camera, the game's own swings give way:
            // the recentre (+0x8E) and the swing behind the hunter after a hit
            // (+0x89) would otherwise pull the camera back every update. The
            // game's D-pad turn skips them the same way.
            if (held) {
                memory.store8(camera + kSnap, 0u);
                memory.store8(camera + kMhp2gFollowFlag, 0u);
            }
            mhp2g.pitch = std::clamp(mhp2g.pitch + turn.pitch_degrees, -90.0f, 90.0f);
            // Where the eye is, from the last update: its height above the
            // look-at point (the game eases it towards its target) and how far
            // it stands from it on the ground.
            const float eye_dy = load_float(memory, camera + 4u) - load_float(memory, camera + 0x14u);
            const float horizontal = std::hypot(load_float(memory, camera) - load_float(memory, camera + 0x10u),
                                                load_float(memory, camera + 8u) - load_float(memory, camera + 0x18u));
            mhp2g.eye_dy = std::isfinite(eye_dy) ? eye_dy : 0.0f;
            // A wall the eye backs into pulls it to the hunter's back. The more
            // it is pulled in, the higher the eye goes, up to kMhp2gWallRise
            // degrees, so the camera looks over the hunter instead of into it;
            // eased both ways. TUTIKUMO_NO_CAMERA_RISE=1 leaves the game's pull.
            static const bool no_rise = std::getenv("TUTIKUMO_NO_CAMERA_RISE") != nullptr;
            float wanted_rise = 0.0f;
            const std::uint32_t preset = memory.load32(camera + kPreset);
            if (!no_rise && std::isfinite(horizontal) && memory.raw_pointer(preset, kMhp2gPresetBytes) != nullptr) {
                // Measured against the ground distance the eye should have at
                // its height, so a camera turned to look down, whose eye stands
                // nearer on purpose, is not taken for one a wall pulled in.
                const float reach = std::fabs(load_float(memory, preset + kMhp2gPresetDistance));
                const float dy = std::clamp(mhp2g.eye_dy, -0.9f * reach, 0.9f * reach);
                const float expected = std::sqrt(std::max(reach * reach - dy * dy, 0.0f));
                if (expected > 1.0f) {
                    const float pulled = std::clamp((0.7f * expected - horizontal) / (0.45f * expected), 0.0f, 1.0f);
                    wanted_rise = kMhp2gWallRise * pulled;
                }
            }
            mhp2g.rise += (wanted_rise - mhp2g.rise) * 0.15f;
            if (mhp2g.rise < 0.05f) mhp2g.rise = 0.0f;
            // The preset is chosen part way through the update, so the eye
            // is turned once it has been (mhp2g_state_check). It is also
            // turned while the eye's height still settles after a turn ended,
            // so the distance holds until the eye is back where the game had it.
            const bool settling = mhp2g.turned && std::fabs(mhp2g.eye_dy - mhp2g.level_dy) > 4.0f;
            if ((mhp2g.pitch != 0.0f || mhp2g.rise > 0.0f || settling) && mhp2g.state_check)
                mhp2g.armed_camera = camera;
            else
                mhp2g.turned = false;
            // Positive yaw turns right; the game's D-pad left adds to the yaw.
            const float wanted = mhp2g.yaw_remainder - turn.yaw_degrees * kAngleUnits;
            const int step = static_cast<int>(wanted);
            mhp2g.yaw_remainder = turn.yaw_held ? wanted - static_cast<float>(step) : 0.0f;
            if (step != 0) {
                for (const std::uint32_t field : {kYawTarget, kYawCurrent}) {
                    const auto yaw = static_cast<std::uint16_t>(memory.load16(camera + field) + step);
                    memory.store16(camera + field, yaw);
                }
            }
        }
    }
    // The update may not finish inside this call: generated code hands back to
    // the dispatcher part way through a long function, which then carries on
    // from there. So the turn of the eye stays armed until the state check
    // uses it, and the game's own preset goes back at the next update or flip.
    mhp2g.original(runtime, ctx);
}

// MHP2G: the hunter state check (0x08865CB0). The camera update calls it four
// times after choosing its preset; when the last call (0x088874DC) returns 0
// the update reads the eye's offset from the preset in the next instruction.
// Then, and only then, the camera is given a copy of its preset whose offset
// is turned about the look-at point by the player's pitch, keeping its length.
void mhp2g_state_check(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t return_address = ctx.gpr[31];
    const std::uint32_t camera = ctx.gpr[20];  // s4 in the camera update
    mhp2g.state_check(runtime, ctx);
    if (return_address != kMhp2gPresetRead || mhp2g.armed_camera == 0u || camera != mhp2g.armed_camera ||
        ctx.gpr[2] != 0u)
        return;
    auto &memory = runtime.memory();
    const std::uint32_t preset = memory.load32(camera + kPreset);
    if (preset == kMhp2gPresetCopy || memory.raw_pointer(preset, kMhp2gPresetBytes) == nullptr) return;
    // The preset's height is the eye's above the hunter's feet, the look-at
    // point is +0x10 above them, and its distance is on the ground. The game
    // takes the distance at once but eases the eye's height towards its
    // target over many updates, so a turn that set both would pull the eye in
    // while it climbs (traced: half the distance at 60 degrees). The target
    // height goes where the turned eye should be, and the ground distance
    // follows the height the eye has now, keeping it on a sphere about the
    // look-at point.
    const float height = load_float(memory, preset + kMhp2gPresetHeight);
    const float distance = load_float(memory, preset + kMhp2gPresetDistance);
    const float look_at = load_float(memory, preset + kMhp2gPresetLookAt);
    const float level_dy = height - look_at;
    const float radius = std::hypot(distance, level_dy);
    if (!std::isfinite(radius) || !std::isfinite(look_at) || radius <= 1.0f) return;
    const float preset_elevation = std::atan2(level_dy, std::fabs(distance)) / kRadians;
    const float lowest = std::min(kMhp2gLowestElevation, preset_elevation);
    const float highest = std::max(kMhp2gHighestElevation, preset_elevation);
    const float player_elevation = std::clamp(preset_elevation + mhp2g.pitch, lowest, highest);
    // What the clamp kept: pushing past a limit stores nothing.
    mhp2g.pitch = player_elevation - preset_elevation;
    const float elevation = std::min(player_elevation + mhp2g.rise, highest);
    const float now_dy = std::clamp(mhp2g.eye_dy, -0.97f * radius, 0.97f * radius);
    const float ground = std::max(std::sqrt(radius * radius - now_dy * now_dy),
                                  radius * std::cos(kMhp2gHighestElevation * kRadians));
    for (std::uint32_t i = 0; i < kMhp2gPresetBytes; i += 4u)
        memory.store32(kMhp2gPresetCopy + i, memory.load32(preset + i));
    store_float(memory, kMhp2gPresetCopy + kMhp2gPresetHeight, look_at + radius * std::sin(elevation * kRadians));
    store_float(memory, kMhp2gPresetCopy + kMhp2gPresetDistance, std::copysign(ground, distance));
    mhp2g.level_dy = level_dy;
    mhp2g.turned = true;
    memory.store32(camera + kPreset, kMhp2gPresetCopy);
    mhp2g.game_preset = preset;
    mhp2g.preset_camera = camera;
    mhp2g.armed_camera = 0u;
}

void camera_rotation(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    adjust_camera(runtime, ctx);
    // Continue the original helper with the same CPU context and return PC.
    // Do not invoke an isolated guest call: normal scheduling must be retained.
    state.original(runtime, ctx);
}

} // namespace

std::span<const CodeWord> game_code_signature() { return kSignature; }

std::span<const CodeWord> mhp2g_camera_signature() { return kMhp2gSignature; }

bool prepare_mhp2g_camera(psprecomp::Runtime &runtime, RotationFunction original, RotationFunction state_check) {
    state = State{};
    mhp2g = Mhp2gState{};
    reset();
    lock_on_reset();
    if (!original || !runtime.has_function(kMhp2gCameraUpdate)) {
        std::cerr << "[camera] the MHP2G camera update is not in the generated code; analog camera unavailable\n";
        return false;
    }
    for (const CodeWord &expected : kMhp2gSignature) {
        const std::uint32_t found = runtime.memory().load32(expected.address);
        if (found == expected.word) continue;
        std::cerr << "[camera] game code differs at 0x" << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(8) << expected.address << ": 0x" << std::setw(8) << found << ", expected 0x"
                  << std::setw(8) << expected.word << std::dec << std::nouppercase << std::setfill(' ') << " ("
                  << expected.what << "); analog camera unavailable\n";
        return false;
    }
    mhp2g.original = original;
    if (state_check && runtime.has_function(kMhp2gStateCheck)) mhp2g.state_check = state_check;
    else std::cerr << "[camera] the MHP2G state check is not in the generated code; no vertical camera\n";
    mhp2g.prepared = true;
    std::cout << "[camera] MHP2G analog camera ready" << (mhp2g.state_check ? ", with pitch" : "") << "\n";
    return true;
}

bool prepare_game_camera(psprecomp::Runtime &runtime, RotationFunction original) {
    state = State{};
    reset();
    lock_on_reset();
    if (!original || !runtime.has_function(kRotationHelper)) {
        std::cerr << "[camera] the rotation helper is not in the generated code; analog camera unavailable\n";
        return false;
    }
    for (const CodeWord &expected : kSignature) {
        const std::uint32_t found = runtime.memory().load32(expected.address);
        if (found == expected.word) continue;
        std::cerr << "[camera] game code differs at 0x" << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(8) << expected.address << ": 0x" << std::setw(8) << found << ", expected 0x"
                  << std::setw(8) << expected.word << std::dec << std::nouppercase << std::setfill(' ') << " ("
                  << expected.what << "); analog camera unavailable\n";
        return false;
    }
    state.original = original;
    state.prepared = true;
    return true;
}

void game_camera_frame(psprecomp::Runtime &runtime) {
    ++state.frame;
    trace_flip(runtime.memory());
    // A step taken off in advance that no aim update has put back: the aim
    // code and the camera did not run, so it goes back now.
    settle_anticipation(runtime.memory());
    // A lock that is on, or a tap waiting, needs the hook as well; a player
    // who never taps pays nothing for lock-on either.
    if (mhp2g.prepared) {
        mhp2g_restore_preset(runtime.memory());
        // TUTIKUMO_TRACE_CAMERA: the camera as the frame shows it. Every frame
        // while the stick is held or after a hit, otherwise twice a second.
        static const bool trace = std::getenv("TUTIKUMO_TRACE_CAMERA") != nullptr;
        if (trace && mhp2g.last_camera != 0u && state.frame - state.last_update <= 1u) {
            auto &memory = runtime.memory();
            const std::uint32_t c = mhp2g.last_camera;
            const unsigned hit = memory.load8(c + kMhp2gHitTimer);
            if (mhp2g.last_held || hit != 0u || state.frame % 15u == 0u) {
                const float ex = load_float(memory, c), ey = load_float(memory, c + 4u), ez = load_float(memory, c + 8u);
                const float ax = load_float(memory, c + 0x10u), ay = load_float(memory, c + 0x14u),
                            az = load_float(memory, c + 0x18u);
                const float dx = ex - ax, dy = ey - ay, dz = ez - az;
                const float horizontal = std::hypot(dx, dz);
                const std::uint32_t preset = memory.load32(c + kPreset);
                std::cout << "[cam2g] f=" << state.frame << " held=" << mhp2g.last_held << " in=" << mhp2g.last_yaw
                          << ',' << mhp2g.last_pitch_input << " pitch=" << mhp2g.pitch << " rise=" << mhp2g.rise << " dist="
                          << std::hypot(horizontal, dy) << " elev=" << std::atan2(dy, horizontal) / kRadians
                          << " eye=" << ex << ',' << ey << ',' << ez << " at=" << ax << ',' << ay << ',' << az
                          << " yaw=" << std::hex << memory.load16(c + kYawTarget) << '/' << memory.load16(c + kYawCurrent)
                          << " 89=" << static_cast<unsigned>(memory.load8(c + kMhp2gFollowFlag)) << " 8b=" << hit
                          << " 8d=" << static_cast<unsigned>(memory.load8(c + 0x8Du))
                          << " 8e=" << static_cast<unsigned>(memory.load8(c + kSnap)) << " preset=" << preset << std::dec
                          << " h,d=" << load_float(memory, preset + kMhp2gPresetHeight) << ','
                          << load_float(memory, preset + kMhp2gPresetDistance)
                          << " fov=" << load_float(memory, c + kMhp2gFieldOfView) << '\n';
            }
        }
        // MHP3rd's lock-on reads that game's monsters, so it stays off here.
        if (!state.hooked && option_on()) {
            // As below: only at the flip, with no generated frame live.
            runtime.register_function(kMhp2gCameraUpdate, &mhp2g_camera_update, "mhp2g_camera_update");
            if (mhp2g.state_check)
                runtime.register_function(kMhp2gStateCheck, &mhp2g_state_check, "mhp2g_state_check");
            state.hooked = true;
        }
        if (state.frame - state.last_update > 1u) {
            state.available = false;
            mhp2g.yaw_remainder = 0.0f;
        }
        if (!game_camera_driving()) discard();
        return;
    }
    if (!lock_on_allowed()) lock_on_release("");
    lock_on_frame(runtime.memory());
    if (state.prepared && !state.hooked && (option_on() || (lock_on_allowed() && lock_on_wanted()))) {
        // Only ever at the game's flip, from an import: no generated frame is
        // live on the host stack, so the dispatch tables can change here.
        // A host registration disables this unit's direct-call shortcut, so the
        // generated cross-unit camera call reaches the wrapper without
        // regeneration. It stays until exit; with the option off again the
        // wrapper only passes through.
        runtime.register_function(kRotationHelper, &camera_rotation, "tutikumo_camera_rotation");
        state.hooked = true;
    }
    if (state.frame - state.last_update > 1u) release();
    if (!driving_allowed() && !(state.pitch_by_lock && lock_on_status().locked)) {
        state.pitch_owned = false;
        state.yaw_remainder = 0.0f;
    }
    // Nothing takes the input while the camera update is not running.
    if (!game_camera_driving()) discard();
}

bool game_camera_driving() {
    return (driving_allowed() || (state.hooked && lock_on_status().locked)) && state.available;
}

bool game_camera_aim_boost() {
    return driving_allowed() && state.aiming && state.frame - state.last_update <= 1u;
}

std::optional<StickDirection> game_camera_mouse_aim() {
    const auto direction = mouse_direction();
    if (!direction) return std::nullopt;
    const auto [x, y] = *direction;
    if (std::ofstream *out = aim_trace(); out != nullptr && state.mouse_shown != state.frame + 1u)
        *out << state.frame << ",show," << x << ':' << y << '\n';
    state.mouse_shown = state.frame + 1u;
    state.shown_x = x;
    state.shown_y = y;
    const float length = std::hypot(static_cast<float>(x), static_cast<float>(y));
    return StickDirection{static_cast<float>(x) / length, static_cast<float>(y) / length};
}

void game_camera_anticipate_aim(psprecomp::Runtime &runtime) {
    auto &memory = runtime.memory();
    settle_anticipation(memory);
    const auto direction = mouse_direction();
    const std::uint32_t hunter = state.aim_hunter;
    if (!direction || memory.raw_pointer(hunter, kHunterExtent) == nullptr) return;
    // A held stick is shown to the game instead of the mouse.
    for (const Source source : {Source::Stick, Source::Keys, Source::Touch}) {
        const Rate held = rate(source);
        if (held.yaw != 0.0f || held.pitch != 0.0f) return;
    }
    const auto [x, y] = *direction;
    const std::size_t diagonal = x != 0 && y != 0 ? 1u : 0u;
    // A step size not seen yet in this aim is taken from the other one: the
    // game's diagonal steps are its straight ones times cos 45 degrees.
    const auto step_for = [diagonal](const std::array<int, 2> &steps) {
        if (steps[diagonal] != 0) return steps[diagonal];
        constexpr float kDiagonal = 0.70710678f;
        const float other = static_cast<float>(steps[1u - diagonal]);
        return static_cast<int>(std::lround(diagonal != 0u ? other * kDiagonal : other / kDiagonal));
    };
    const auto heading = memory.load16(hunter + kHunterHeading);
    const int yaw_step = step_for(state.yaw_step);
    if (x != 0 && state.yaw_sign != 0 && yaw_step != 0 && heading == state.aim_heading) {
        state.anticipated_yaw = state.yaw_sign * x * yaw_step;
        state.anticipated_heading = static_cast<std::uint16_t>(heading - state.anticipated_yaw);
        store_heading(memory, hunter, state.anticipated_heading);
    }
    const PitchField &field = kPitchFields[state.pitch_field];
    const int pitch = load_pitch(memory, hunter, field);
    const int pitch_step = step_for(state.pitch_step);
    if (y != 0 && state.pitch_sign != 0 && pitch_step != 0 && pitch == state.aim_pitch[state.pitch_field]) {
        const int ahead = std::clamp(pitch - state.pitch_sign * y * pitch_step, -field.limit, field.limit);
        state.anticipated_pitch = pitch - ahead;
        state.anticipated_pitch_value = ahead;
        if (state.anticipated_pitch != 0) store_pitch(memory, hunter, field, ahead);
    }
}

int game_camera_mouse_stock_turn() {
    if (game_camera_driving() || game_camera_aim_boost()) return 0;
    const float yaw = peek_pointer().yaw_degrees;
    if (!(std::fabs(yaw) >= kMouseStockTurn)) return 0;
    return yaw > 0.0f ? 1 : -1;
}

float game_camera_degrees_per_second() {
    const auto &s = settings::current();
    return state.aiming ? s.aim_speed : s.camera_speed;
}

} // namespace tutikumo::camera
