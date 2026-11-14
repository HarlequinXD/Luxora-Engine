/* Game templates for new projects: a First Person Shooter and a Third Person Action game, made with
 * State Graphs (no scripts) so everything is visible and editable in the State Graph editor.
 *
 * Both share: Enemy AI (idle, wander, chase, attack, hurt, die), Player Health, pickups, a Game Manager
 * (HUD, pause menu, score, win and game over) and global variables/events in Assets/Globals.lxglobals. */
#include "../lx_internal.h"
#include <stdio.h>

static bool put(const char *assets, const char *rel, const char *text) {
    char path[LX_PATH_MAX], dir[LX_PATH_MAX];
    lx_path_join(path, sizeof path, assets, rel);
    lx_path_parent(dir, sizeof dir, path);
    lx_mkdir_p(dir);
    return lx_file_write(path, text, strlen(text));
}

/* ======================================================================
 *  graphs shared by both templates
 * ====================================================================== */
static const char GLOBALS[] =
    "# Global variables and events: every State Graph can use them\n"
    "[variable]\nname = Health\ntype = float\nvalue = 100\n"
    "[variable]\nname = Max Health\ntype = float\nvalue = 100\n"
    "[variable]\nname = Score\ntype = int\nvalue = 0\n"
    "[variable]\nname = Ammo\ntype = int\nvalue = 12\n"
    "[variable]\nname = Reserve Ammo\ntype = int\nvalue = 36\n"
    "[variable]\nname = Coins\ntype = int\nvalue = 0\n"
    "[event]\nname = PAUSE\n[event]\nname = RESUME\n[event]\nname = GAME_OVER\n[event]\nname = WIN\n"
    "[event]\nname = ADD_SCORE\n[event]\nname = DAMAGE\n[event]\nname = HIT\n[event]\nname = HEAL\n[event]\nname = AMMO\n";

static const char ENEMY_AI[] =
    "[graph]\nname = Enemy AI\n"
    "description = Waits and wanders near home, chases the player on sight, attacks in reach. HIT events hurt it; it dies at 0 health and adds to the score.\n"
    "start = Wake Up\n"
    "[event]\nname = SEEN\nglobal = 0\n[event]\nname = LOST\nglobal = 0\n[event]\nname = IN_REACH\nglobal = 0\n[event]\nname = ARRIVED\nglobal = 0\n"
    "[event]\nname = DIE\nglobal = 0\n[event]\nname = GIVE_UP\nglobal = 0\n[event]\nname = ALREADY_DEAD\nglobal = 0\n"
    "[event]\nname = HIT\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n"
    "[variable]\nname = Health\ntype = float\nvalue = 60\ninspector = 1\ntooltip = Hit points: each HIT event takes its value off\n"
    "[variable]\nname = Speed\ntype = float\nvalue = 2.8\ninspector = 1\ntooltip = Chase speed in metres per second\n"
    "[variable]\nname = Sight Range\ntype = float\nvalue = 13\ninspector = 1\n"
    "[variable]\nname = Lose Range\ntype = float\nvalue = 22\ninspector = 1\ntooltip = Gives up the chase beyond this distance\n"
    "[variable]\nname = Attack Range\ntype = float\nvalue = 1.7\ninspector = 1\n"
    "[variable]\nname = Attack Damage\ntype = float\nvalue = 10\ninspector = 1\n"
    "[variable]\nname = Score Value\ntype = float\nvalue = 100\ninspector = 1\n"
    "[variable]\nname = Body Material\ntype = asset\nvalue = Materials/Enemy.lxmat\ninspector = 1\n"
    "[variable]\nname = Player\ntype = object\nvalue = None\n"
    "[variable]\nname = Body\ntype = object\nvalue = None\n"
    "[variable]\nname = Home\ntype = vector3\nvalue = 0 0 0\n"
    "[variable]\nname = Wander To\ntype = vector3\nvalue = 0 0 0\n"
    "[variable]\nname = Hit\ntype = float\nvalue = 0\n"
    "[variable]\nname = Is Dead\ntype = bool\nvalue = false\n"
    "[global]\nevent = HIT\nto = Hurt\n"
    "[global]\nevent = GAME_OVER\nto = Celebrate\n"
    "[state]\nname = Wake Up\npos = 80 80\ndescription = Find the player, remember home\ntransition = FINISHED > Idle\n"
    "[action]\ntype = Find Object\nupdate = once\np0 = Tag\np1 = Player\np2 = $Player\n"
    "[action]\ntype = Get Position\nupdate = once\np0 = Owner\np1 = $Home\np2 = World\n"
    "[action]\ntype = Get Child\nupdate = once\np0 = Owner\np1 = Body\np2 = $Body\n"
    "[state]\nname = Idle\npos = 80 240\ndescription = Look around for a moment\ntransition = SEEN > Chase\ntransition = FINISHED > Wander\n"
    "[action]\ntype = Get Distance\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = $Player\np3 = $Sight Range\np4 = SEEN\n"
    "[action]\ntype = Random Wait\np0 = 1.5\np1 = 3.5\n"
    "[state]\nname = Wander\npos = 80 420\ncolor = 7\ndescription = Stroll to a spot near home\ntransition = ARRIVED > Idle\ntransition = GIVE_UP > Idle\ntransition = SEEN > Chase\n"
    "[action]\ntype = Random Point\nupdate = once\np0 = $Home\np1 = 6\np2 = true\np3 = $Wander To\n"
    "[action]\ntype = Smooth Look At\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = None\np2 = $Wander To\np3 = 6\np4 = true\n"
    "[action]\ntype = Move Towards\nupdate = every_frame\np0 = Owner\np1 = None\np2 = $Wander To\np3 = 1.3\np4 = 0.4\np5 = ARRIVED\np6 = true\n"
    "[action]\ntype = Get Distance\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = $Player\np3 = $Sight Range\np4 = SEEN\n"
    "[action]\ntype = Send Event\nfolded = 1\np0 = GIVE_UP\np1 = This Graph\np4 = 6\n"
    "[state]\nname = Chase\npos = 400 240\ncolor = 1\ndescription = Run at the player\ntransition = IN_REACH > Attack\ntransition = LOST > Idle\n"
    "[action]\ntype = Smooth Look At\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = $Player\np3 = 10\np4 = true\n"
    "[action]\ntype = Move Towards\nupdate = every_frame\np0 = Owner\np1 = $Player\np2 = 0 0 0\np3 = $Speed\np4 = $Attack Range\np5 = IN_REACH\np6 = true\n"
    "[action]\ntype = Get Distance\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = $Player\np3 = $Lose Range\np5 = LOST\n"
    "[state]\nname = Attack\npos = 720 240\ncolor = 1\nsequence = 1\ndescription = Lunge and hurt the player, then catch breath\ntransition = FINISHED > Chase\n"
    "[action]\ntype = Look At\nupdate = once\np0 = Owner\np1 = $Player\np3 = true\n"
    "[action]\ntype = Tween Scale\np0 = $Body\np1 = 0.75 1.05 1.3\np2 = 0.12\np3 = Ease Out\n"
    "[action]\ntype = Send Event\np0 = DAMAGE\np1 = Object\np2 = $Player\np5 = $Attack Damage\n"
    "[action]\ntype = Tween Scale\np0 = $Body\np1 = 1 1 1\np2 = 0.2\np3 = Back Out\n"
    "[action]\ntype = Wait\np0 = 0.7\n"
    "[state]\nname = Hurt\npos = 400 460\ncolor = 3\ndescription = Take the hit's value off Health; flash red\ntransition = ALREADY_DEAD > Gone\ntransition = DIE > Dead\ntransition = FINISHED > Recover\n"
    "[action]\ntype = Bool Test\nupdate = once\np0 = $Is Dead\np1 = ALREADY_DEAD\n"
    "[action]\ntype = Get Event Info\nupdate = once\np0 = $Hit\n"
    "[action]\ntype = Float Operator\nupdate = once\np0 = $Health\np1 = $Hit\np2 = Subtract\np3 = $Health\n"
    "[action]\ntype = Float Compare\nupdate = once\np0 = $Health\np1 = 0\np3 = DIE\np4 = DIE\n"
    "[action]\ntype = Set Material\nupdate = once\np0 = $Body\np1 = builtin:red\n"
    "[action]\ntype = Shake\np0 = $Body\np1 = 0.08\np2 = 0.15\n"
    "[state]\nname = Recover\npos = 720 460\ncolor = 3\ntransition = FINISHED > Chase\n"
    "[action]\ntype = Set Material\nupdate = once\np0 = $Body\np1 = $Body Material\n"
    "[state]\nname = Dead\npos = 400 640\ncolor = 7\ndescription = Score, shrink away, go\ntransition = FINISHED > Gone\n"
    "[action]\ntype = Set Bool\nupdate = once\np0 = $Is Dead\np1 = true\n"
    "[action]\ntype = Set Tag\nupdate = once\np0 = Owner\np1 = Dead\n"
    "[action]\ntype = Broadcast Event\np0 = ADD_SCORE\np1 = $Score Value\n"
    "[action]\ntype = Tween Scale\np0 = Owner\np1 = 0.01 0.01 0.01\np2 = 0.35\np3 = Ease In\n"
    "[state]\nname = Gone\npos = 720 640\ncolor = 7\n"
    "[action]\ntype = Destroy Object\np0 = Owner\np1 = 0\n"
    "[state]\nname = Celebrate\npos = 1040 240\ncolor = 4\ndescription = The player is down\n"
    "[action]\ntype = Rotate\nupdate = every_frame\np0 = Owner\np1 = 0 120 0\np2 = true\n"
    "[comment]\nrect = 1000 400 300 140\ncolor = 0\n"
    "text = HIT and GAME_OVER are global transitions: they work from any state. Send HIT with a value (the damage) from a weapon to hurt the enemy.\n";

static const char PLAYER_HEALTH[] =
    "[graph]\nname = Player Health\n"
    "description = DAMAGE events take their value off the global Health; HEAL events add theirs. At 0 the game is over.\n"
    "start = Alive\n"
    "[event]\nname = DIE\n[event]\nname = ALREADY_DEAD\n"
    "[event]\nname = DAMAGE\nglobal = 1\n[event]\nname = HEAL\nglobal = 1\n"
    "[variable]\nname = Camera\ntype = object\nvalue = name:Main Camera\ninspector = 1\ntooltip = Shaken when you're hit\n"
    "[variable]\nname = Amount\ntype = float\nvalue = 0\n"
    "[variable]\nname = Is Dead\ntype = bool\nvalue = false\n"
    "[global]\nevent = DAMAGE\nto = Hurt\n"
    "[global]\nevent = HEAL\nto = Healing\n"
    "[state]\nname = Alive\npos = 80 160\ncolor = 4\n"
    "[state]\nname = Hurt\npos = 380 80\ncolor = 1\ntransition = ALREADY_DEAD > Down\ntransition = DIE > Dead\ntransition = FINISHED > Alive\n"
    "[action]\ntype = Bool Test\nupdate = once\np0 = $Is Dead\np1 = ALREADY_DEAD\n"
    "[action]\ntype = Get Event Info\nupdate = once\np0 = $Amount\n"
    "[action]\ntype = Float Operator\nupdate = once\np0 = $Health\np1 = $Amount\np2 = Subtract\np3 = $Health\n"
    "[action]\ntype = Float Compare\nupdate = once\np0 = $Health\np1 = 0\np3 = DIE\np4 = DIE\n"
    "[action]\ntype = Shake\np0 = $Camera\np1 = 0.06\np2 = 0.25\n"
    "[state]\nname = Healing\npos = 380 260\ncolor = 4\ntransition = ALREADY_DEAD > Down\ntransition = FINISHED > Alive\n"
    "[action]\ntype = Bool Test\nupdate = once\np0 = $Is Dead\np1 = ALREADY_DEAD\n"
    "[action]\ntype = Get Event Info\nupdate = once\np0 = $Amount\n"
    "[action]\ntype = Float Add\nupdate = once\np0 = $Health\np1 = $Amount\n"
    "[action]\ntype = Float Clamp\nupdate = once\np0 = $Health\np1 = 0\np2 = $Max Health\n"
    "[state]\nname = Dead\npos = 700 80\ncolor = 7\ntransition = FINISHED > Down\n"
    "[action]\ntype = Set Bool\nupdate = once\np0 = $Is Dead\np1 = true\n"
    "[action]\ntype = Set Float\nupdate = once\np0 = $Health\np1 = 0\n"
    "[action]\ntype = Broadcast Event\np0 = GAME_OVER\n"
    "[state]\nname = Down\npos = 700 260\ncolor = 7\n";

/* a pickup: bobs and spins until the player comes close, then gives its event and value and goes */
static void pickup_graph(char *out, size_t cap, const char *name, const char *desc, const char *event, const char *amount, bool to_player) {
    snprintf(out, cap,
             "[graph]\nname = %s\ndescription = %s\nstart = Start\n"
             "[event]\nname = TAKEN\n[event]\nname = %s\nglobal = 1\n"
             "[variable]\nname = Amount\ntype = float\nvalue = %s\ninspector = 1\n"
             "[variable]\nname = Pick Up Distance\ntype = float\nvalue = 1.3\ninspector = 1\n"
             "[variable]\nname = Player\ntype = object\nvalue = None\n"
             "[variable]\nname = Start Position\ntype = vector3\nvalue = 0 0 0\n"
             "[variable]\nname = Base Y\ntype = float\nvalue = 0\n"
             "[variable]\nname = Y\ntype = float\nvalue = 0\n"
             "[state]\nname = Start\npos = 80 80\ntransition = FINISHED > Waiting\n"
             "[action]\ntype = Find Object\nupdate = once\np0 = Tag\np1 = Player\np2 = $Player\n"
             "[action]\ntype = Get Position\nupdate = once\np0 = Owner\np1 = $Start Position\np2 = Local\n"
             "[action]\ntype = Get Vector3 XYZ\nupdate = once\np0 = $Start Position\np2 = $Base Y\n"
             "[state]\nname = Waiting\npos = 80 240\ncolor = 5\ntransition = TAKEN > Taken\n"
             "[action]\ntype = Rotate\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = 0 120 0\np2 = true\n"
             "[action]\ntype = Float Oscillate\nupdate = every_frame\nfolded = 1\np0 = $Base Y\np1 = 0.15\np2 = 0.6\np3 = Sine\np4 = $Y\n"
             "[action]\ntype = Set Position Axis\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = Y\np2 = $Y\n"
             "[action]\ntype = Get Distance\nupdate = every_frame\np0 = Owner\np1 = $Player\np3 = $Pick Up Distance\np4 = TAKEN\n"
             "[state]\nname = Taken\npos = 400 240\ncolor = 4\nsequence = 1\ntransition = FINISHED > Gone\n"
             "[action]\ntype = Set Tag\nupdate = once\np0 = Owner\np1 = Taken\n"
             "[action]\ntype = Send Event\np0 = %s\np1 = %s\np2 = $Player\np5 = $Amount\n"
             "[action]\ntype = Tween Scale\np0 = Owner\np1 = 0.01 0.01 0.01\np2 = 0.18\np3 = Back Out\n"
             "[state]\nname = Gone\npos = 400 400\ncolor = 7\n"
             "[action]\ntype = Destroy Object\np0 = Owner\np1 = 0\n",
             name, desc, event, amount, event, to_player ? "Object" : "Everyone");
}

static const char IMPACT[] =
    "[graph]\nname = Impact\ndescription = A bullet hit: flash and fade\nstart = Flash\n"
    "[state]\nname = Flash\npos = 80 80\ntransition = FINISHED > Gone\n"
    "[action]\ntype = Tween Scale\np0 = Owner\np1 = 0.01 0.01 0.01\np2 = 0.22\np3 = Ease In\n"
    "[state]\nname = Gone\npos = 80 240\n"
    "[action]\ntype = Destroy Object\np0 = Owner\np1 = 0\n";

/* the Game Manager: HUD (template-specific lines), pause menu, score, win and game over */
static void game_manager(char *out, size_t cap, bool fps) {
    const char *hud = fps ?
        "[action]\ntype = Build String\nupdate = every_frame\nfolded = 1\np0 = {0}  /  {1}\np1 = $Ammo\np2 = $Reserve Ammo\np3 = $Ammo Text\n"
        "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Ammo Text\np1 = -60 -110 0\np2 = 64\np3 = 1 1 1 1\np4 = Bottom Right\n"
        "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = AMMO\np1 = -60 -60 0\np2 = 30\np3 = 1 0.8 0.5 0.8\np4 = Bottom Right\n"
        "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = +\np1 = 0 0 0\np2 = 46\np3 = 1 1 1 0.85\np4 = Center\n"
        "[action]\ntype = Build String\nupdate = every_frame\nfolded = 1\np0 = Enemies left  {0}\np1 = $Left\np3 = $Goal Text\n"
        "[action]\ntype = Count With Tag\nupdate = every_frame\nfolded = 1\np0 = Enemy\np1 = $Left\n"
        :
        "[action]\ntype = Build String\nupdate = every_frame\nfolded = 1\np0 = Coins  {0} / {1}\np1 = $Coins\np2 = $Coins Total\np3 = $Goal Text\n"
        "[action]\ntype = Count With Tag\nupdate = every_frame\nfolded = 1\np0 = Coin\np1 = $Left\n"
        "[action]\ntype = Int Operator\nupdate = every_frame\nfolded = 1\np0 = $Coins Total\np1 = $Left\np2 = Subtract\np3 = $Coins\n";
    snprintf(out, cap,
             "[graph]\nname = Game Manager\n"
             "description = The HUD, the pause menu (Esc / Start), the score, winning (%s) and game over.\n"
             "start = Start\n"
             "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n[event]\nname = WIN\nglobal = 1\n"
             "[event]\nname = ADD_SCORE\nglobal = 1\n[event]\nname = RESUME_CLICKED\n[event]\nname = RESTART\n[event]\nname = QUIT\n[event]\nname = OPEN_MENU\n[event]\nname = ALL_DONE\n"
             "[variable]\nname = Left\ntype = int\nvalue = 0\n"
             "[variable]\nname = %s\ntype = int\nvalue = 0\n"
             "[variable]\nname = Got\ntype = int\nvalue = 0\n"
             "[variable]\nname = Health Text\ntype = string\nvalue = \n"
             "[variable]\nname = Score Text\ntype = string\nvalue = \n"
             "[variable]\nname = Ammo Text\ntype = string\nvalue = \n"
             "[variable]\nname = Goal Text\ntype = string\nvalue = \n"
             "[global]\nevent = GAME_OVER\nto = Game Over\n"
             "[state]\nname = Start\npos = 80 80\ndescription = Count what there is to %s\ntransition = FINISHED > Playing\n"
             "[action]\ntype = Count With Tag\nupdate = once\np0 = %s\np1 = %s\n"
             "[state]\nname = Playing\npos = 80 260\ncolor = 2\ndescription = Draw the HUD every frame\n"
             "transition = OPEN_MENU > Paused\ntransition = ALL_DONE > Victory\ntransition = ADD_SCORE > Add Score\n"
             "[action]\ntype = UI Bar\nupdate = every_frame\nfolded = 1\np0 = $Health\np1 = $Max Health\np2 = 40 -60 0\np3 = 420\np4 = 0.85 0.2 0.1 1\np5 = Bottom Left\n"
             "[action]\ntype = Build String\nupdate = every_frame\nfolded = 1\np0 = {0}\np1 = $Health\np3 = $Health Text\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Health Text\np1 = 44 -100 0\np2 = 40\np3 = 1 1 1 1\np4 = Bottom Left\n"
             "[action]\ntype = Build String\nupdate = every_frame\nfolded = 1\np0 = Score  {0}\np1 = $Score\np3 = $Score Text\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Score Text\np1 = -40 40 0\np2 = 46\np3 = 1 1 1 1\np4 = Top Right\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Goal Text\np1 = -40 100 0\np2 = 34\np3 = 1 0.85 0.6 1\np4 = Top Right\n"
             "%s"
             "[action]\ntype = Int Compare\nupdate = every_frame\nfolded = 1\np0 = $Left\np1 = 0\np2 = ALL_DONE\n"
             "[action]\ntype = Input Action Event\nupdate = every_frame\np0 = Pause\np1 = Pressed\np2 = OPEN_MENU\n"
             "[state]\nname = Add Score\npos = 400 420\ncolor = 4\ntransition = FINISHED > Playing\n"
             "[action]\ntype = Get Event Info\nupdate = once\np0 = $Got\n"
             "[action]\ntype = Int Add\nupdate = once\np0 = $Score\np1 = $Got\n"
             "[state]\nname = Paused\npos = 400 80\ncolor = 3\ndescription = Freeze time and show the menu\n"
             "transition = RESUME_CLICKED > Resume\ntransition = RESTART > Restart\ntransition = QUIT > Quit\n"
             "[action]\ntype = Set Time Scale\nupdate = once\np0 = 0\n"
             "[action]\ntype = Broadcast Event\np0 = PAUSE\n"
             "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
             "[action]\ntype = UI Panel\nupdate = every_frame\nfolded = 1\np0 = 0 0 0\np1 = 4000 3000 0\np2 = 0 0 0 0.55\np3 = 0\np4 = Top Left\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = PAUSED\np1 = 0 -200 0\np2 = 110\np3 = 1 1 1 1\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Resume\np1 = 0 -20 0\np2 = 360 90 0\np3 = RESUME_CLICKED\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Restart\np1 = 0 90 0\np2 = 360 90 0\np3 = RESTART\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Quit\np1 = 0 200 0\np2 = 360 90 0\np3 = QUIT\np4 = Center\n"
             "[action]\ntype = Input Action Event\nupdate = every_frame\np0 = Pause\np1 = Pressed\np2 = RESUME_CLICKED\n"
             "[state]\nname = Resume\npos = 720 80\ncolor = 3\ntransition = FINISHED > Playing\n"
             "[action]\ntype = Set Time Scale\nupdate = once\np0 = 1\n"
             "[action]\ntype = Broadcast Event\np0 = RESUME\n"
             "[state]\nname = Restart\npos = 720 240\ncolor = 7\n"
             "[action]\ntype = Set Time Scale\nupdate = once\np0 = 1\n"
             "[action]\ntype = Restart Scene\n"
             "[state]\nname = Quit\npos = 720 400\ncolor = 7\n"
             "[action]\ntype = Quit Game\n"
             "[state]\nname = Victory\npos = 80 620\ncolor = 4\ndescription = Everything done\ntransition = RESTART > Restart\ntransition = QUIT > Quit\n"
             "[action]\ntype = Broadcast Event\np0 = WIN\n"
             "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
             "[action]\ntype = UI Panel\nupdate = every_frame\nfolded = 1\np0 = 0 0 0\np1 = 4000 3000 0\np2 = 0.02 0.08 0.03 0.6\np3 = 0\np4 = Top Left\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = %s\np1 = 0 -220 0\np2 = 100\np3 = 0.6 1 0.6 1\np4 = Center\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Score Text\np1 = 0 -100 0\np2 = 56\np3 = 1 1 1 1\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Play Again\np1 = 0 40 0\np2 = 360 90 0\np3 = RESTART\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Quit\np1 = 0 150 0\np2 = 360 90 0\np3 = QUIT\np4 = Center\n"
             "[state]\nname = Game Over\npos = 400 620\ncolor = 1\ndescription = The player died\ntransition = RESTART > Restart\ntransition = QUIT > Quit\n"
             "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
             "[action]\ntype = Screen Fade\nupdate = every_frame\np0 = Out\np1 = 1.5\np2 = 0.25 0 0 0.75\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = YOU DIED\np1 = 0 -200 0\np2 = 110\np3 = 1 0.3 0.25 1\np4 = Center\n"
             "[action]\ntype = UI Text\nupdate = every_frame\nfolded = 1\np0 = $Score Text\np1 = 0 -90 0\np2 = 56\np3 = 1 1 1 1\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Try Again\np1 = 0 40 0\np2 = 360 90 0\np3 = RESTART\np4 = Center\n"
             "[action]\ntype = UI Button\nupdate = every_frame\np0 = Quit\np1 = 0 150 0\np2 = 360 90 0\np3 = QUIT\np4 = Center\n"
             "[comment]\nrect = 1040 80 320 170\ncolor = 0\n"
             "text = Score, Health, Ammo and Coins are global variables (Variables tab > Global Variables), so every graph can change them. ADD_SCORE is broadcast by enemies and coins with the points as its value.\n",
             fps ? "defeat every enemy" : "collect every coin",
             fps ? "Enemies Total" : "Coins Total",
             fps ? "beat" : "collect",
             fps ? "Enemy" : "Coin", fps ? "$Enemies Total" : "$Coins Total",
             hud,
             fps ? "AREA CLEARED" : "ALL COINS FOUND");
}

/* ======================================================================
 *  first person
 * ====================================================================== */
static const char FPS_CONTROLLER[] =
    "[graph]\nname = FPS Controller\n"
    "description = Mouse look, walking, running (Shift) and jumping. Paused and finished games give the mouse back.\n"
    "start = Playing\n"
    "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n[event]\nname = WIN\nglobal = 1\n"
    "[variable]\nname = Walk Speed\ntype = float\nvalue = 4.5\ninspector = 1\n"
    "[variable]\nname = Run Multiplier\ntype = float\nvalue = 1.7\ninspector = 1\n"
    "[variable]\nname = Jump Height\ntype = float\nvalue = 1.2\ninspector = 1\n"
    "[variable]\nname = Mouse Sensitivity\ntype = float\nvalue = 1\ninspector = 1\n"
    "[variable]\nname = Camera\ntype = object\nvalue = name:Main Camera\ninspector = 1\n"
    "[global]\nevent = PAUSE\nto = Paused\n[global]\nevent = GAME_OVER\nto = Stopped\n[global]\nevent = WIN\nto = Stopped\n"
    "[state]\nname = Playing\npos = 80 120\ncolor = 2\ndescription = Lock the mouse, look and move\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Lock\np1 = Don't Change\n"
    "[action]\ntype = First Person Look\nupdate = every_frame\np0 = Owner\np1 = $Camera\np2 = $Mouse Sensitivity\np4 = LookX\np5 = LookY\np6 = -85\np7 = 85\n"
    "[action]\ntype = Character Walk\nupdate = every_frame\np0 = Owner\np1 = MoveX\np2 = MoveY\np3 = Owner\np4 = $Walk Speed\np5 = Sprint\np6 = $Run Multiplier\np7 = Jump\n"
    "p8 = $Jump Height\np9 = 20\np10 = false\np11 = 12\n"
    "[state]\nname = Paused\npos = 400 40\ncolor = 3\ntransition = RESUME > Playing\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
    "[state]\nname = Stopped\npos = 400 200\ncolor = 7\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
    "[action]\ntype = Tween Rotation\np0 = $Camera\np1 = -10 0 25\np2 = 0.6\np3 = Bounce Out\n";

static const char FPS_WEAPON[] =
    "[graph]\nname = FPS Weapon\n"
    "description = Hitscan rifle: hold Fire to shoot, R to reload. A hit sends HIT (value = Damage) to what it hit. AMMO pickups refill the reserve.\n"
    "start = Ready\n"
    "[event]\nname = FIRE\n[event]\nname = RELOAD\n[event]\nname = SHOOT\n[event]\nname = EMPTY\n[event]\nname = HIT_SOMETHING\n[event]\nname = NEED\n[event]\nname = FULL\n"
    "[event]\nname = CAN\n[event]\nname = NONE\n"
    "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n[event]\nname = WIN\nglobal = 1\n[event]\nname = AMMO\nglobal = 1\n"
    "[variable]\nname = Damage\ntype = float\nvalue = 25\ninspector = 1\n"
    "[variable]\nname = Fire Delay\ntype = float\nvalue = 0.13\ninspector = 1\ntooltip = Seconds between shots\n"
    "[variable]\nname = Range\ntype = float\nvalue = 120\ninspector = 1\n"
    "[variable]\nname = Clip Size\ntype = int\nvalue = 12\ninspector = 1\n"
    "[variable]\nname = Camera\ntype = object\nvalue = name:Main Camera\ninspector = 1\n"
    "[variable]\nname = Gun\ntype = object\nvalue = name:Gun\ninspector = 1\n"
    "[variable]\nname = Muzzle Light\ntype = object\nvalue = name:Muzzle Light\ninspector = 1\n"
    "[variable]\nname = Target\ntype = object\nvalue = None\n"
    "[variable]\nname = Hit Point\ntype = vector3\nvalue = 0 0 0\n"
    "[variable]\nname = Need\ntype = int\nvalue = 0\n"
    "[variable]\nname = Take\ntype = int\nvalue = 0\n"
    "[variable]\nname = Got\ntype = int\nvalue = 0\n"
    "[global]\nevent = PAUSE\nto = Holstered\n[global]\nevent = GAME_OVER\nto = Holstered\n[global]\nevent = WIN\nto = Holstered\n[global]\nevent = AMMO\nto = Pick Up Ammo\n"
    "[state]\nname = Ready\npos = 80 160\ncolor = 2\ntransition = FIRE > Check Ammo\ntransition = RELOAD > Check Reload\n"
    "[action]\ntype = Input Action Event\nupdate = every_frame\np0 = Fire\np1 = Held\np2 = FIRE\n"
    "[action]\ntype = Key Event\nupdate = every_frame\np0 = R\np1 = Pressed\np2 = RELOAD\n"
    "[state]\nname = Check Ammo\npos = 380 80\ntransition = SHOOT > Shoot\ntransition = EMPTY > Empty\n"
    "[action]\ntype = Int Compare\nupdate = once\np0 = $Ammo\np1 = 0\np2 = EMPTY\np3 = EMPTY\np4 = SHOOT\n"
    "[state]\nname = Shoot\npos = 680 80\ncolor = 1\ndescription = Flash, kick, fire a ray from the camera\ntransition = HIT_SOMETHING > Hit Something\ntransition = FINISHED > Cooldown\n"
    "[action]\ntype = Int Add\nupdate = once\np0 = $Ammo\np1 = -1\n"
    "[action]\ntype = Set Light\nupdate = once\np0 = $Muzzle Light\np1 = 1 0.7 0.35 1\np2 = 40\n"
    "[action]\ntype = Shake\np0 = $Gun\np1 = 0.025\np2 = 0.08\n"
    "[action]\ntype = Set Object\nupdate = once\np0 = $Target\np1 = None\n"
    "[action]\ntype = Raycast\nupdate = once\np0 = $Camera\np1 = 0 0 0\np2 = 0 0 -1\np3 = Self\np4 = $Range\np5 = HIT_SOMETHING\np8 = $Target\np9 = $Hit Point\n"
    "[state]\nname = Hit Something\npos = 980 80\ncolor = 1\ntransition = FINISHED > Cooldown\n"
    "[action]\ntype = Send Event\np0 = HIT\np1 = Object\np2 = $Target\np5 = $Damage\n"
    "[action]\ntype = Spawn Prefab\nupdate = once\np0 = Prefabs/Impact.lxprefab\np1 = None\np2 = $Hit Point\n"
    "[state]\nname = Cooldown\npos = 680 240\nsequence = 1\ndescription = Let the flash show for a moment, then wait out the fire delay\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Wait\np0 = 0.04\n"
    "[action]\ntype = Set Light\nupdate = once\np0 = $Muzzle Light\np1 = 1 0.7 0.35 1\np2 = 0\n"
    "[action]\ntype = Wait\np0 = $Fire Delay\n"
    "[state]\nname = Empty\npos = 380 240\ncolor = 7\ndescription = Click... reload if there's ammo left\ntransition = RELOAD > Check Reload\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Int Compare\nupdate = once\np0 = $Reserve Ammo\np1 = 0\np4 = RELOAD\n"
    "[action]\ntype = Wait\np0 = 0.3\n"
    "[state]\nname = Check Reload\npos = 80 380\ntransition = NEED > Check Reserve\ntransition = FULL > Ready\n"
    "[action]\ntype = Int Compare\nupdate = once\np0 = $Ammo\np1 = $Clip Size\np2 = FULL\np3 = NEED\np4 = FULL\n"
    "[state]\nname = Check Reserve\npos = 380 400\ntransition = CAN > Reloading\ntransition = NONE > Ready\n"
    "[action]\ntype = Int Compare\nupdate = once\np0 = $Reserve Ammo\np1 = 0\np2 = NONE\np3 = NONE\np4 = CAN\n"
    "[state]\nname = Reloading\npos = 680 400\ncolor = 6\nsequence = 1\ntransition = FINISHED > Refill\n"
    "[action]\ntype = Tween Rotation\np0 = $Gun\np1 = 35 -20 0\np2 = 0.25\np3 = Ease Out\n"
    "[action]\ntype = Wait\np0 = 0.8\n"
    "[state]\nname = Refill\npos = 980 400\ncolor = 6\ndescription = Move bullets from the reserve into the clip\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Int Operator\nupdate = once\np0 = $Clip Size\np1 = $Ammo\np2 = Subtract\np3 = $Need\n"
    "[action]\ntype = Int Operator\nupdate = once\np0 = $Need\np1 = $Reserve Ammo\np2 = Min\np3 = $Take\n"
    "[action]\ntype = Int Add\nupdate = once\np0 = $Ammo\np1 = $Take\n"
    "[action]\ntype = Int Operator\nupdate = once\np0 = $Reserve Ammo\np1 = $Take\np2 = Subtract\np3 = $Reserve Ammo\n"
    "[action]\ntype = Tween Rotation\np0 = $Gun\np1 = 0 0 0\np2 = 0.2\np3 = Back Out\n"
    "[state]\nname = Pick Up Ammo\npos = 80 560\ncolor = 4\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Get Event Info\nupdate = once\np0 = $Got\n"
    "[action]\ntype = Int Add\nupdate = once\np0 = $Reserve Ammo\np1 = $Got\n"
    "[state]\nname = Holstered\npos = 380 560\ncolor = 7\ntransition = RESUME > Ready\n"
    "[action]\ntype = Set Light\nupdate = once\np0 = $Muzzle Light\np1 = 1 0.7 0.35 1\np2 = 0\n";

/* ======================================================================
 *  third person
 * ====================================================================== */
static const char TPS_CONTROLLER[] =
    "[graph]\nname = TPS Controller\n"
    "description = Walks where the camera points, turns to face the way it goes, runs (Shift) and jumps.\n"
    "start = Playing\n"
    "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n[event]\nname = WIN\nglobal = 1\n"
    "[variable]\nname = Walk Speed\ntype = float\nvalue = 5\ninspector = 1\n"
    "[variable]\nname = Run Multiplier\ntype = float\nvalue = 1.6\ninspector = 1\n"
    "[variable]\nname = Jump Height\ntype = float\nvalue = 1.6\ninspector = 1\n"
    "[variable]\nname = Camera\ntype = object\nvalue = name:Main Camera\ninspector = 1\n"
    "[global]\nevent = PAUSE\nto = Paused\n[global]\nevent = GAME_OVER\nto = Stopped\n[global]\nevent = WIN\nto = Stopped\n"
    "[state]\nname = Playing\npos = 80 200\ncolor = 2\ndescription = Lock the mouse and move\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Lock\np1 = Don't Change\n"
    "[action]\ntype = Character Walk\nupdate = every_frame\np0 = Owner\np1 = MoveX\np2 = MoveY\np3 = $Camera\np4 = $Walk Speed\np5 = Sprint\np6 = $Run Multiplier\np7 = Jump\n"
    "p8 = $Jump Height\np9 = 20\np10 = true\np11 = 12\n"
    "[state]\nname = Paused\npos = 400 40\ncolor = 3\ntransition = RESUME > Playing\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n"
    "[state]\nname = Stopped\npos = 400 360\ncolor = 7\n"
    "[action]\ntype = Set Mouse\nupdate = once\np0 = Unlock\np1 = Show\n";

static const char TPS_CAMERA[] =
    "[graph]\nname = TPS Camera\n"
    "description = The orbit camera: mouse / right stick to look around the player. Stays still while paused.\n"
    "start = Follow\n"
    "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n"
    "[variable]\nname = Distance\ntype = float\nvalue = 5.5\ninspector = 1\n"
    "[variable]\nname = Height\ntype = float\nvalue = 1.7\ninspector = 1\n"
    "[variable]\nname = Sensitivity\ntype = float\nvalue = 1\ninspector = 1\n"
    "[global]\nevent = PAUSE\nto = Paused\n"
    "[state]\nname = Follow\npos = 80 120\ncolor = 2\n"
    "[action]\ntype = Third Person Camera\nupdate = late\np0 = Owner\np1 = tag:Player\np2 = $Distance\np3 = $Height\np4 = $Sensitivity\np5 = LookX\np6 = LookY\np7 = -30\np8 = 70\np9 = 12\np10 = true\n"
    "[state]\nname = Paused\npos = 400 120\ncolor = 3\ntransition = RESUME > Follow\n";

static const char TPS_ATTACK[] =
    "[graph]\nname = TPS Attack\n"
    "description = Fire swings the sword; the nearest enemy within Reach gets a HIT (value = Damage).\n"
    "start = Start\n"
    "[event]\nname = ATTACK\n[event]\nname = IN_REACH\n"
    "[event]\nname = PAUSE\nglobal = 1\n[event]\nname = RESUME\nglobal = 1\n[event]\nname = GAME_OVER\nglobal = 1\n"
    "[variable]\nname = Damage\ntype = float\nvalue = 30\ninspector = 1\n"
    "[variable]\nname = Reach\ntype = float\nvalue = 2.4\ninspector = 1\n"
    "[variable]\nname = Sword\ntype = object\nvalue = None\n"
    "[variable]\nname = Target\ntype = object\nvalue = None\n"
    "[variable]\nname = Distance\ntype = float\nvalue = 99\n"
    "[global]\nevent = PAUSE\nto = Sheathed\n[global]\nevent = GAME_OVER\nto = Sheathed\n"
    "[state]\nname = Start\npos = 80 40\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Get Child\nupdate = once\np0 = Owner\np1 = Sword\np2 = $Sword\n"
    "[state]\nname = Ready\npos = 80 200\ncolor = 2\ntransition = ATTACK > Swing\n"
    "[action]\ntype = Input Action Event\nupdate = every_frame\np0 = Fire\np1 = Pressed\np2 = ATTACK\n"
    "[state]\nname = Swing\npos = 380 200\ncolor = 1\ndescription = Swing; is an enemy close enough?\ntransition = IN_REACH > Strike\ntransition = FINISHED > Recover\n"
    "[action]\ntype = Tween Rotation\np0 = $Sword\np1 = 0 -110 -20\np2 = 0.12\np3 = Ease Out\n"
    "[action]\ntype = Set Float\nupdate = once\np0 = $Distance\np1 = 99\n"
    "[action]\ntype = Find Closest With Tag\nupdate = once\np0 = Owner\np1 = Enemy\np2 = $Target\np3 = $Distance\n"
    "[action]\ntype = Float Compare\nupdate = once\np0 = $Distance\np1 = $Reach\np4 = IN_REACH\n"
    "[state]\nname = Strike\npos = 680 200\ncolor = 1\ntransition = FINISHED > Recover\n"
    "[action]\ntype = Send Event\np0 = HIT\np1 = Object\np2 = $Target\np5 = $Damage\n"
    "[action]\ntype = Shake\np0 = name:Main Camera\np1 = 0.04\np2 = 0.12\n"
    "[state]\nname = Recover\npos = 380 360\ntransition = FINISHED > Ready\n"
    "[action]\ntype = Tween Rotation\np0 = $Sword\np1 = 0 0 0\np2 = 0.22\np3 = Back Out\n"
    "[state]\nname = Sheathed\npos = 80 360\ncolor = 7\ntransition = RESUME > Ready\n";

/* ======================================================================
 *  materials, prefabs, levels
 * ====================================================================== */
static void material(const char *assets, const char *name, LxVec3 albedo, float metallic, float roughness, LxVec3 emissive, float glow) {
    LxMaterial m = lx_material_default();
    lx_strcpy(m.name, sizeof m.name, name);
    m.albedo = lx_v4(albedo.x, albedo.y, albedo.z, 1);
    m.metallic = metallic;
    m.roughness = roughness;
    m.emissive = emissive;
    m.emissive_strength = glow;
    char rel[LX_REF_MAX], path[LX_PATH_MAX];
    snprintf(rel, sizeof rel, "Materials/%s.lxmat", name);
    lx_path_join(path, sizeof path, assets, rel);
    lx_material_save(&m, path);
}

static LxEntity prim(LxScene *s, LxPrimitive p, const char *name, LxVec3 pos, LxVec3 scale, const char *mat, bool collider) {
    LxEntity e = lx_spawn_primitive(s, p, name, pos);
    lx_entity_set_scale(s, e, scale);
    LxMeshRenderer *m = lx_get_mesh(s, e);
    if (m && mat) lx_strcpy(m->material, LX_REF_MAX, mat);
    if (!collider) lx_remove_component(s, e, LX_COMP_COLLIDER);
    return e;
}

/* the level both games play in: a walled yard with cover, a raised walkway and ramps */
static void arena(LxScene *s, bool tps) {
    LxEnvironment *env = lx_scene_environment(s);
    env->sky_zenith = lx_v3(0.20f, 0.30f, 0.52f);
    env->sky_horizon = lx_v3(0.78f, 0.62f, 0.50f);
    env->fog_color = lx_v3(0.62f, 0.55f, 0.52f);
    env->fog_density = 0.008f;
    LxEntity sun = lx_entity_find(s, "Directional Light");
    if (sun) {
        lx_entity_set_euler(s, sun, lx_v3(-38, -40, 0));
        LxLight *l = lx_get_light(s, sun);
        l->color = lx_v3(1.0f, 0.88f, 0.74f);
        l->intensity = 3.2f;
    }
    LxEntity cube = lx_entity_find(s, "Cube");
    if (cube) lx_entity_destroy(s, cube);
    LxEntity ground = prim(s, LX_PRIM_PLANE, "Ground", lx_v3(0, 0, 0), lx_v3(5, 1, 5), "Materials/Floor.lxmat", true);
    lx_entity_set_tag(s, ground, "Ground");
    const float half = 24;
    prim(s, LX_PRIM_CUBE, "Wall North", lx_v3(0, 2, -half), lx_v3(2 * half, 4, 1), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall South", lx_v3(0, 2, half), lx_v3(2 * half, 4, 1), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall West", lx_v3(-half, 2, 0), lx_v3(1, 4, 2 * half), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall East", lx_v3(half, 2, 0), lx_v3(1, 4, 2 * half), "Materials/Wall.lxmat", true);
    /* cover: crates and pillars */
    static const float crates[][4] = {{-6, -4, 1.4f, 20}, {-5, -2.2f, 1.0f, -10}, {7, 5, 1.6f, 35}, {8.5f, 3.4f, 1.0f, 5}, {-12, 9, 1.5f, 0},
                                      {3, -12, 1.4f, 15}, {14, -9, 1.2f, -25}, {-15, -14, 1.6f, 40}, {0, 10, 1.2f, 10}};
    for (size_t i = 0; i < sizeof crates / sizeof crates[0]; i++) {
        char n[32];
        snprintf(n, sizeof n, "Crate %d", (int)i + 1);
        float sz = crates[i][2];
        LxEntity c = prim(s, LX_PRIM_CUBE, n, lx_v3(crates[i][0], sz * 0.5f, crates[i][1]), lx_v3(sz, sz, sz), "Materials/Crate.lxmat", true);
        lx_entity_set_euler(s, c, lx_v3(0, crates[i][3], 0));
    }
    static const float pillars[][2] = {{-9, -9}, {9, -9}, {-9, 9}, {9, 9}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Pillar %d", i + 1);
        prim(s, LX_PRIM_CYLINDER, n, lx_v3(pillars[i][0], 2.5f, pillars[i][1]), lx_v3(1.2f, 5, 1.2f), "Materials/Wall.lxmat", true);
    }
    /* a walkway along the north wall with a ramp up to it, and steps on the east side */
    prim(s, LX_PRIM_CUBE, "Walkway", lx_v3(-6, 1.4f, -20.5f), lx_v3(20, 0.4f, 5), "Materials/Metal.lxmat", true);
    LxEntity ramp = prim(s, LX_PRIM_CUBE, "Ramp", lx_v3(7.2f, 0.72f, -20.5f), lx_v3(7, 0.3f, 4), "Materials/Metal.lxmat", true);
    lx_entity_set_euler(s, ramp, lx_v3(0, 0, 12));
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Step %d", i + 1);
        float h = 0.35f * (float)(i + 1);
        prim(s, LX_PRIM_CUBE, n, lx_v3(19, h * 0.5f, 6.0f - (float)i * 2.2f), lx_v3(4, h, 2.2f), "Materials/Crate.lxmat", true);
    }
    if (tps) {   /* platforms to jump across for coins */
        static const float plats[][4] = {{-14, 0, 1.0f, 3}, {-18, -4, 1.8f, 3}, {-14, -9, 2.6f, 3}, {-8, -13, 3.2f, 3.4f}, {12, 14, 1.2f, 3}, {16, 18, 2.2f, 3}};
        for (size_t i = 0; i < sizeof plats / sizeof plats[0]; i++) {
            char n[32];
            snprintf(n, sizeof n, "Platform %d", (int)i + 1);
            prim(s, LX_PRIM_CUBE, n, lx_v3(plats[i][0], plats[i][2] - 0.2f, plats[i][1]), lx_v3(plats[i][3], 0.4f, plats[i][3]), "Materials/Metal.lxmat", true);
        }
    }
    /* warm lamps */
    static const float lamps[][3] = {{-9, 3.5f, -9}, {9, 3.5f, 9}, {-12, 3.0f, 12}, {12, 3.0f, -12}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Lamp %d", i + 1);
        LxEntity l = lx_entity_create(s, n);
        lx_entity_set_position(s, l, lx_v3(lamps[i][0], lamps[i][1], lamps[i][2]));
        LxLight *li = lx_add_light(s, l, LX_LIGHT_POINT);
        li->color = lx_v3(1.0f, 0.62f, 0.32f);
        li->intensity = 6;
        li->range = 12;
    }
    LxEntity probe = lx_entity_create(s, "Reflection Probe");
    lx_entity_set_position(s, probe, lx_v3(0, 3, 0));
    lx_add_reflection_probe(s, probe)->size = lx_v3(2 * half, 8, 2 * half);
}

static LxEntity enemy(LxScene *s, const char *name, LxVec3 at) {
    LxEntity e = lx_entity_create(s, name);
    lx_entity_set_position(s, e, at);
    lx_entity_set_tag(s, e, "Enemy");
    LxCharacterController *cc = lx_add_character(s, e);
    cc->radius = 0.45f;
    cc->height = 1.8f;
    LxCollider *col = lx_add_collider(s, e, LX_COLLIDER_CAPSULE);   /* what bullets and swords hit */
    col->center = lx_v3(0, 0.9f, 0);
    col->radius = 0.45f;
    col->height = 1.8f;
    LxEntity body = prim(s, LX_PRIM_CAPSULE, "Body", lx_v3(0, 0.9f, 0), lx_v3(0.9f, 0.9f, 0.9f), "Materials/Enemy.lxmat", false);
    lx_entity_set_parent(s, body, e);
    LxEntity eye = prim(s, LX_PRIM_SPHERE, "Eye", lx_v3(0, 0.45f, -0.42f), lx_v3(0.35f, 0.18f, 0.12f), "Materials/Glow Red.lxmat", false);
    lx_entity_set_parent(s, eye, body);
    lx_add_state_graph(s, e, "Graphs/Enemy AI.lxgraph");
    return e;
}

static void pickup(LxScene *s, const char *name, const char *graph, const char *mat, const char *tag, LxVec3 at, LxPrimitive shape, LxVec3 scale) {
    LxEntity e = prim(s, shape, name, at, scale, mat, false);
    lx_entity_set_tag(s, e, tag);
    lx_add_state_graph(s, e, graph);
}

static LxEntity manager(LxScene *s) {
    LxEntity gm = lx_entity_create(s, "Game Manager");
    lx_add_state_graph(s, gm, "Graphs/Game Manager.lxgraph");
    return gm;
}

static void common_assets(const char *assets, bool fps) {
    material(assets, "Floor", lx_v3(0.36f, 0.36f, 0.34f), 0, 0.55f, lx_v3(0, 0, 0), 0);
    material(assets, "Wall", lx_v3(0.62f, 0.56f, 0.50f), 0, 0.8f, lx_v3(0, 0, 0), 0);
    material(assets, "Crate", lx_v3(0.55f, 0.36f, 0.20f), 0, 0.7f, lx_v3(0, 0, 0), 0);
    material(assets, "Metal", lx_v3(0.55f, 0.57f, 0.60f), 1, 0.3f, lx_v3(0, 0, 0), 0);
    material(assets, "Enemy", lx_v3(0.30f, 0.10f, 0.12f), 0.2f, 0.45f, lx_v3(0, 0, 0), 0);
    material(assets, "Glow Red", lx_v3(1, 0.2f, 0.1f), 0, 0.4f, lx_v3(1, 0.15f, 0.05f), 6);
    material(assets, "Health", lx_v3(0.2f, 0.9f, 0.3f), 0, 0.3f, lx_v3(0.2f, 1, 0.3f), 2.5f);
    material(assets, "Player", lx_v3(0.18f, 0.32f, 0.62f), 0.1f, 0.4f, lx_v3(0, 0, 0), 0);
    put(assets, "Globals.lxglobals", GLOBALS);
    put(assets, "Graphs/Enemy AI.lxgraph", ENEMY_AI);
    put(assets, "Graphs/Player Health.lxgraph", PLAYER_HEALTH);
    static char buf[16384];
    pickup_graph(buf, sizeof buf, "Health Pickup", "Bobs until the player is close, then heals them by Amount", "HEAL", "25", true);
    put(assets, "Graphs/Health Pickup.lxgraph", buf);
    game_manager(buf, sizeof buf, fps);
    put(assets, "Graphs/Game Manager.lxgraph", buf);
}

/* ---------------- First Person Shooter ---------------- */
static bool fps_template(const char *assets, LxScene *s) {
    common_assets(assets, true);
    material(assets, "Ammo", lx_v3(0.95f, 0.75f, 0.2f), 0.6f, 0.35f, lx_v3(1, 0.7f, 0.15f), 1.5f);
    material(assets, "Gun", lx_v3(0.08f, 0.08f, 0.09f), 0.9f, 0.35f, lx_v3(0, 0, 0), 0);
    material(assets, "Spark", lx_v3(1, 0.8f, 0.4f), 0, 0.5f, lx_v3(1, 0.7f, 0.3f), 8);
    put(assets, "Graphs/FPS Controller.lxgraph", FPS_CONTROLLER);
    put(assets, "Graphs/FPS Weapon.lxgraph", FPS_WEAPON);
    put(assets, "Graphs/Impact.lxgraph", IMPACT);
    static char buf[16384];
    pickup_graph(buf, sizeof buf, "Ammo Pickup", "Bobs until the player is close, then gives them Amount bullets", "AMMO", "24", true);
    put(assets, "Graphs/Ammo Pickup.lxgraph", buf);
    /* the bullet impact prefab */
    {
        LxScene *ps = lx_scene_create("Impact");
        LxEntity sp = prim(ps, LX_PRIM_SPHERE, "Impact", lx_v3(0, 0, 0), lx_v3(0.18f, 0.18f, 0.18f), "Materials/Spark.lxmat", false);
        lx_add_state_graph(ps, sp, "Graphs/Impact.lxgraph");
        char pp[LX_PATH_MAX];
        lx_path_join(pp, sizeof pp, assets, "Prefabs/Impact.lxprefab");
        lx_prefab_save(ps, sp, pp);
        lx_scene_destroy(ps);
    }
    arena(s, false);
    /* the player: a Character Controller with the camera at eye height and a gun in view */
    LxEntity cam = lx_entity_find(s, "Main Camera");
    LxEntity p = lx_entity_create(s, "Player");
    lx_entity_set_position(s, p, lx_v3(0, 0, 16));
    lx_entity_set_tag(s, p, "Player");
    LxCharacterController *cc = lx_add_character(s, p);
    cc->radius = 0.4f;
    cc->height = 1.8f;
    if (cam) {
        lx_entity_set_parent(s, cam, p);
        lx_entity_set_position(s, cam, lx_v3(0, 1.62f, 0));
        lx_entity_set_euler(s, cam, lx_v3(0, 0, 0));
        lx_get_camera(s, cam)->fov_deg = 72;
        LxEntity gun = prim(s, LX_PRIM_CUBE, "Gun", lx_v3(0.26f, -0.24f, -0.5f), lx_v3(0.09f, 0.12f, 0.55f), "Materials/Gun.lxmat", false);
        lx_entity_set_parent(s, gun, cam);
        LxEntity barrel = prim(s, LX_PRIM_CYLINDER, "Barrel", lx_v3(0, 0.02f, -0.62f), lx_v3(0.5f, 0.35f, 0.5f), "Materials/Metal.lxmat", false);
        lx_entity_set_euler(s, barrel, lx_v3(90, 0, 0));
        lx_entity_set_parent(s, barrel, gun);
        LxEntity ml = lx_entity_create(s, "Muzzle Light");
        lx_entity_set_parent(s, ml, cam);
        lx_entity_set_position(s, ml, lx_v3(0.26f, -0.18f, -1.0f));
        LxLight *l = lx_add_light(s, ml, LX_LIGHT_POINT);
        l->color = lx_v3(1, 0.7f, 0.35f);
        l->intensity = 0;
        l->range = 6;
        l->cast_shadows = false;
    }
    lx_add_state_graph(s, p, "Graphs/FPS Controller.lxgraph");
    lx_add_state_graph(s, p, "Graphs/FPS Weapon.lxgraph");
    lx_add_state_graph(s, p, "Graphs/Player Health.lxgraph");
    static const float en[][2] = {{-8, -6}, {6, -8}, {12, 2}, {-14, 4}, {0, -16}, {16, -16}};
    for (int i = 0; i < 6; i++) {
        char n[32];
        snprintf(n, sizeof n, "Enemy %d", i + 1);
        enemy(s, n, lx_v3(en[i][0], 0, en[i][1]));
    }
    pickup(s, "Health Pack 1", "Graphs/Health Pickup.lxgraph", "Materials/Health.lxmat", "Pickup", lx_v3(-18, 0.6f, -18), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Health Pack 2", "Graphs/Health Pickup.lxgraph", "Materials/Health.lxmat", "Pickup", lx_v3(18, 0.6f, 12), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Ammo Box 1", "Graphs/Ammo Pickup.lxgraph", "Materials/Ammo.lxmat", "Pickup", lx_v3(-6, 2.2f, -20.5f), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    pickup(s, "Ammo Box 2", "Graphs/Ammo Pickup.lxgraph", "Materials/Ammo.lxmat", "Pickup", lx_v3(10, 0.6f, 10), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    pickup(s, "Ammo Box 3", "Graphs/Ammo Pickup.lxgraph", "Materials/Ammo.lxmat", "Pickup", lx_v3(19, 1.9f, -0.6f), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    manager(s);
    return true;
}

/* ---------------- Third Person Action ---------------- */
static bool tps_template(const char *assets, LxScene *s) {
    common_assets(assets, false);
    material(assets, "Gold", lx_v3(1.0f, 0.78f, 0.3f), 1, 0.25f, lx_v3(0.6f, 0.4f, 0.05f), 0.8f);
    material(assets, "Blade", lx_v3(0.85f, 0.88f, 0.92f), 1, 0.15f, lx_v3(0, 0, 0), 0);
    put(assets, "Graphs/TPS Controller.lxgraph", TPS_CONTROLLER);
    put(assets, "Graphs/TPS Camera.lxgraph", TPS_CAMERA);
    put(assets, "Graphs/TPS Attack.lxgraph", TPS_ATTACK);
    static char buf[16384];
    pickup_graph(buf, sizeof buf, "Coin", "Spins until the player is close, then adds Amount to the score", "ADD_SCORE", "50", false);
    put(assets, "Graphs/Coin.lxgraph", buf);
    arena(s, true);
    LxEntity p = lx_entity_create(s, "Player");
    lx_entity_set_position(s, p, lx_v3(0, 0, 14));
    lx_entity_set_tag(s, p, "Player");
    LxCharacterController *cc = lx_add_character(s, p);
    cc->radius = 0.4f;
    cc->height = 1.8f;
    LxEntity body = prim(s, LX_PRIM_CAPSULE, "Body", lx_v3(0, 0.9f, 0), lx_v3(0.8f, 0.9f, 0.8f), "Materials/Player.lxmat", false);
    lx_entity_set_parent(s, body, p);
    LxEntity visor = prim(s, LX_PRIM_CUBE, "Visor", lx_v3(0, 0.42f, -0.36f), lx_v3(0.55f, 0.16f, 0.2f), "Materials/Metal.lxmat", false);
    lx_entity_set_parent(s, visor, body);
    LxEntity sword = lx_entity_create(s, "Sword");   /* a pivot at the hand: the blade swings around it */
    lx_entity_set_parent(s, sword, p);
    lx_entity_set_position(s, sword, lx_v3(0.45f, 1.0f, -0.1f));
    LxEntity blade = prim(s, LX_PRIM_CUBE, "Blade", lx_v3(0, 0, -0.55f), lx_v3(0.06f, 0.12f, 1.1f), "Materials/Blade.lxmat", false);
    lx_entity_set_parent(s, blade, sword);
    lx_add_state_graph(s, p, "Graphs/TPS Controller.lxgraph");
    lx_add_state_graph(s, p, "Graphs/TPS Attack.lxgraph");
    lx_add_state_graph(s, p, "Graphs/Player Health.lxgraph");
    LxEntity cam = lx_entity_find(s, "Main Camera");
    if (cam) {
        lx_entity_set_position(s, cam, lx_v3(0, 3.5f, 20));
        lx_get_camera(s, cam)->fov_deg = 60;
        lx_add_state_graph(s, cam, "Graphs/TPS Camera.lxgraph");
    }
    static const float en[][2] = {{-8, -6}, {8, -8}, {12, 4}, {-12, 6}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Enemy %d", i + 1);
        enemy(s, n, lx_v3(en[i][0], 0, en[i][1]));
    }
    static const float coins[][3] = {{0, 0.8f, 6}, {-5, 0.8f, 0}, {5, 0.8f, -3}, {-14, 1.8f, 0}, {-18, 2.6f, -4}, {-14, 3.4f, -9}, {-8, 4.0f, -13},
                                     {-6, 2.4f, -20.5f}, {12, 2.0f, 14}, {16, 3.0f, 18}, {19, 2.2f, -0.6f}, {-20, 0.8f, 18}};
    for (size_t i = 0; i < sizeof coins / sizeof coins[0]; i++) {
        char n[32];
        snprintf(n, sizeof n, "Coin %d", (int)i + 1);
        LxEntity c = prim(s, LX_PRIM_CYLINDER, n, lx_v3(coins[i][0], coins[i][1], coins[i][2]), lx_v3(0.6f, 0.08f, 0.6f), "Materials/Gold.lxmat", false);
        lx_entity_set_euler(s, c, lx_v3(90, 0, 0));
        lx_entity_set_tag(s, c, "Coin");
        lx_add_state_graph(s, c, "Graphs/Coin.lxgraph");
    }
    pickup(s, "Health Pack 1", "Graphs/Health Pickup.lxgraph", "Materials/Health.lxmat", "Pickup", lx_v3(-18, 0.6f, 14), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Health Pack 2", "Graphs/Health Pickup.lxgraph", "Materials/Health.lxmat", "Pickup", lx_v3(18, 0.6f, -14), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    manager(s);
    return true;
}

bool lx_project_template_build(int tpl, const char *assets, LxScene *s) {
    if (tpl == LX_TEMPLATE_FPS) return fps_template(assets, s);
    if (tpl == LX_TEMPLATE_THIRD_PERSON) return tps_template(assets, s);
    return false;
}
