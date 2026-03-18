#pragma once
#include "m_msg.h"
#include "m_actor.h"

/* Called from mMsg_MainSetup_Appear() once per fresh dialogue.
   Starts an async API call; the buffer is NOT modified on return.
   Falls back silently to original ROM text on error or non-villager actors. */
extern void pc_claude_intercept_message(mMsg_Data_c* msg_data, ACTOR* speaker,
                                        int msg_index);

/* Returns 1 while a background API call is in flight, 0 otherwise.
   Called from mMsg_Main_Appear() each frame to suppress the CURSOL transition
   until the response is ready. */
extern int pc_claude_is_async_pending(void);

/* Polls the background thread.  If it has finished, fills the text buffer and
   returns 1 (caller should allow the CURSOL transition).
   Returns 0 if the thread is still running.
   win is the active dialogue window (needed for mMsg_Count_MsgData). */
extern int pc_claude_poll_async(mMsg_Window_c* win);

/* Called from mMsg_end_to_disappear when Claude dialogue fully closes.
   Opens the in-game keyboard so the player can type a response. */
extern void pc_claude_on_dialogue_end(GAME* game);

/* Called every frame from mMsg_Main_Hide. If the player typed a response,
   re-opens the dialogue window to continue the conversation.
   Returns 1 if a re-open was triggered, 0 otherwise. */
extern int pc_claude_check_continue(mMsg_Window_c* msg_p, GAME* game);

/* Returns 1 while a Claude back-and-forth conversation is active.
   Used by mMsg_Check_main_hide() to prevent the demo system from unlocking
   the player/NPC during the keyboard input phase. */
extern int pc_claude_is_conversation_active(void);
