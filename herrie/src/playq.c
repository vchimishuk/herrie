/*
 * Copyright (c) 2006-2007 Ed Schouten <ed@fxq.nl>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/**
 * @file playq.c
 * @brief Playlist handling.
 */

#include "audio_file.h"
#include "audio_output.h"
#include "config.h"
#include "gui.h"
#include "playq_modules.h"

/**
 * @brief Routines that should be used to control the playlist.
 */
struct playq_funcs {
	/**
	 * @brief Give a song that should be played.
	 */
	struct vfsref *(*give)(void);
	/**
	 * @brief Report that playback thread is going idle.
	 */
	void (*idle)(void);
	/**
	 * @brief Select a specific song for playback.
	 */
	int (*select)(struct vfsref *vr);
	/**
	 * @brief Specify that the next song should be played.
	 */
	int (*next)(void);
	/**
	 * @brief Specify that the previous song should be played.
	 */
	int (*prev)(void);
	/**
	 * @brief Notify that a song is about to be removed.
	 */
	void (*notify_pre_removal)(struct vfsref *vr);
	/**
	 * @brief Start when adding first song to the playlist.
	 */
	int autostart;
};

/**
 * @brief Traditional Herrie-style playlist handling.
 */
static struct playq_funcs party_funcs = {
	playq_party_give,
	playq_party_idle,
	playq_party_select,
	playq_party_next,
	playq_party_prev,
	playq_party_notify_pre_removal,
	1,
};
/**
 * @brief XMMS-style playlist handling.
 */
static struct playq_funcs regular_funcs = {
	playq_regular_give,
	playq_regular_idle,
	playq_regular_select,
	playq_regular_next,
	playq_regular_prev,
	playq_regular_notify_pre_removal,
	0,
};
/**
 * @brief Currenty used playlist handling routines.
 */
static struct playq_funcs *funcs = &regular_funcs;

struct vfslist		playq_list = VFSLIST_INITIALIZER;
GMutex 			*playq_lock;
/**
 * @brief Conditional variable used to kick the playq alive when it was
 *        waiting for a new song to be added to the playlist or when
 *        paused.
 */
static GCond		*playq_wakeup;
/**
 * @brief Reference to playback thread.
 */
static GThread		*playq_runner;
/**
 * @brief Flags the playback thread should honour. Writing to them
 *        should be locked down.
 */
static volatile int	playq_flags = 0;
int			playq_repeat = 1;
/**
 * @brief Quit the playback thread.
 */
#define PF_QUIT		0x01
/**
 * @brief Pause the current song.
 */
#define PF_PAUSE	0x02
/**
 * @brief Perform an absolute seek.
 */
#define PF_SEEK_ABS	0x08
/**
 * @brief Perform a relative seek.
 */
#define PF_SEEK_REL	0x10
/**
 * @brief Test whether a seek is performed at all.
 */
#define PF_SEEK		(PF_SEEK_ABS|PF_SEEK_REL)
/**
 * @brief Skip to the next song.
 */
#define PF_SKIP		0x20
/**
 * @brief Amount of seconds which the current song should seek.
 */
static volatile int	playq_seek_time;

/**
 * @brief Infinitely play music in the playlist, honouring the
 *        playq_flags.
 */
static void *
playq_runner_thread(void *unused)
{
	struct vfsref		*nvr;
	struct audio_file	*cur;
	char			*errmsg;

	playq_flags = PF_PAUSE;

	gui_input_sigmask();

	do {
		/* Wait until there's a song available */
		PLAYQ_LOCK;
		while ((!funcs->autostart && playq_flags & PF_PAUSE) ||
		    (nvr = funcs->give()) == NULL) {
			/* Change the current status to idle */
			funcs->idle();
			gui_playq_song_update(NULL, 0, 0);

			g_cond_wait(playq_wakeup, playq_lock);
			if (playq_flags & PF_QUIT) {
				PLAYQ_UNLOCK;
				goto done;
			}
		}
		PLAYQ_UNLOCK;

		cur = audio_file_open(nvr);
		if (cur == NULL) {
			/* Skip broken songs */
			errmsg = g_strdup_printf(
			    _("Failed to open \"%s\" for playback."),
			    vfs_name(nvr));
			gui_msgbar_warn(errmsg);
			g_free(errmsg);
			vfs_close(nvr);
			continue;
		}

		/* Trash it */
		vfs_close(nvr);
		gui_playq_song_update(cur, 0, 0);

		PLAYQ_LOCK;
		playq_flags &= ~(PF_PAUSE|PF_SKIP|PF_SEEK);
		PLAYQ_UNLOCK;

		do  {
			if (playq_flags & PF_PAUSE && !cur->stream) {
				gui_playq_song_update(cur, 1, 1);

				/* Wait to be waken up */
				PLAYQ_LOCK;
				g_cond_wait(playq_wakeup, playq_lock);
				PLAYQ_UNLOCK;
			} else {
				gui_playq_song_update(cur, 0, 1);

				/* Play a part of the audio file */
				if (audio_output_play(cur) <= 0)
					break;
			}

			if (playq_flags & PF_SEEK) {
				audio_file_seek(cur, playq_seek_time,
				    playq_flags & PF_SEEK_REL);
				PLAYQ_LOCK;
				playq_flags &= ~PF_SEEK;
				PLAYQ_UNLOCK;
			}
		} while (!(playq_flags & (PF_QUIT|PF_SKIP)));

		audio_file_close(cur);
	} while (!(playq_flags & PF_QUIT));
done:
	return (NULL);
}

void
playq_init(int party)
{
	playq_lock = g_mutex_new();
	playq_wakeup = g_cond_new();

	if (party || config_getopt_bool("playq.party")) {
		funcs = &party_funcs;
		playq_repeat = 0;
	}
}

void
playq_spawn(void)
{
	playq_runner = g_thread_create_full(playq_runner_thread, NULL,
	    0, 1, TRUE, G_THREAD_PRIORITY_URGENT, NULL);
}

void
playq_shutdown(void)
{
	PLAYQ_LOCK;
	playq_flags = PF_QUIT;
	PLAYQ_UNLOCK;
	g_cond_signal(playq_wakeup);
	g_thread_join(playq_runner);
}

/*
 * Public queue functions: playq_song_*
 */

void
playq_song_add_head(struct vfsref *vr)
{
	/* Recursively expand the item */
	struct vfslist newlist = VFSLIST_INITIALIZER;
	vfs_unfold(&newlist, vr);

	PLAYQ_LOCK;
	/* Copy the expanded contents to the playlist */
	while ((vr = vfs_list_last(&newlist)) != NULL) {
		vfs_list_remove(&newlist, vr);
		vfs_list_insert_head(&playq_list, vr);
		gui_playq_notify_post_insertion(1);
	}

	gui_playq_notify_done();
	g_cond_signal(playq_wakeup);
	PLAYQ_UNLOCK;
}

void
playq_song_add_tail(struct vfsref *vr)
{
	/* Recursively expand the item */
	struct vfslist newlist = VFSLIST_INITIALIZER;
	vfs_unfold(&newlist, vr);

	PLAYQ_LOCK;
	/* Copy the expanded contents to the playlist */
	while ((vr = vfs_list_first(&newlist)) != NULL) {
		vfs_list_remove(&newlist, vr);
		vfs_list_insert_tail(&playq_list, vr);
		gui_playq_notify_post_insertion(vfs_list_items(&playq_list));
	}

	gui_playq_notify_done();
	g_cond_signal(playq_wakeup);
	PLAYQ_UNLOCK;
}

/**
 * @brief Seek the current song by a certain amount of time.
 */
void
playq_cursong_seek(int len, int rel)
{
	int fl;

	fl = rel ? PF_SEEK_REL : PF_SEEK_ABS;
	PLAYQ_LOCK;
	playq_flags = (playq_flags & ~PF_SEEK) | fl;
	playq_seek_time = len;
	PLAYQ_UNLOCK;

	g_cond_signal(playq_wakeup);
}

void
playq_cursong_next(void)
{
	PLAYQ_LOCK;
	if (funcs->next() == 0) {
		/* Unpause as well */
		playq_flags = (playq_flags & ~PF_PAUSE) | PF_SKIP;
		g_cond_signal(playq_wakeup);
	}
	PLAYQ_UNLOCK;
}

void
playq_cursong_prev(void)
{
	PLAYQ_LOCK;
	if (funcs->prev() == 0) {
		/* Unpause as well */
		playq_flags = (playq_flags & ~PF_PAUSE) | PF_SKIP;
		g_cond_signal(playq_wakeup);
	}
	PLAYQ_UNLOCK;
}

void
playq_cursong_stop(void)
{
	PLAYQ_LOCK;
	/* Stop playback */
	playq_flags |= PF_SKIP | PF_PAUSE;
	PLAYQ_UNLOCK;

	g_cond_signal(playq_wakeup);
}

void
playq_cursong_pause(void)
{
	PLAYQ_LOCK;
	/* Toggle the pause flag */
	playq_flags ^= PF_PAUSE;
	PLAYQ_UNLOCK;

	g_cond_signal(playq_wakeup);
}

void
playq_repeat_toggle(void)
{
	char *msg;

	playq_repeat = !playq_repeat;

	msg = g_strdup_printf(_("Repeat: %s"),
	    playq_repeat ? _("on") : _("off"));
	gui_msgbar_warn(msg);
	g_free(msg);
}

void
playq_song_fast_remove(struct vfsref *vr, unsigned int index)
{
	funcs->notify_pre_removal(vr);
	gui_playq_notify_pre_removal(index);
	vfs_list_remove(&playq_list, vr);
	vfs_close(vr);
	gui_playq_notify_done();
}

void
playq_song_fast_add_before(struct vfsref *nvr, struct vfsref *lvr,
    unsigned int index)
{
	/* Recursively expand the item */
	struct vfslist newlist = VFSLIST_INITIALIZER;
	vfs_unfold(&newlist, nvr);

	/* Copy the expanded contents to the playlist */
	while ((nvr = vfs_list_first(&newlist)) != NULL) {
		vfs_list_remove(&newlist, nvr);
		vfs_list_insert_before(&playq_list, nvr, lvr);
		gui_playq_notify_post_insertion(index);
	}

	gui_playq_notify_done();
	g_cond_signal(playq_wakeup);
}

void
playq_song_fast_add_after(struct vfsref *nvr, struct vfsref *lvr,
    unsigned int index)
{
	/* Recursively expand the item */
	struct vfslist newlist = VFSLIST_INITIALIZER;
	vfs_unfold(&newlist, nvr);

	/* Copy the expanded contents to the playlist */
	while ((nvr = vfs_list_last(&newlist)) != NULL) {
		vfs_list_remove(&newlist, nvr);
		vfs_list_insert_after(&playq_list, nvr, lvr);
		gui_playq_notify_post_insertion(index + 1);
	}

	gui_playq_notify_done();
	g_cond_signal(playq_wakeup);
}

void
playq_song_fast_moveup(struct vfsref *vr, unsigned int index)
{
	struct vfsref *pvr;
	
	/* Remove the item above */
	pvr = vfs_list_prev(vr);
	gui_playq_notify_pre_removal(index - 1);
	vfs_list_remove(&playq_list, pvr);

	/* Add it below */
	vfs_list_insert_after(&playq_list, pvr, vr);
	gui_playq_notify_post_insertion(index);
	gui_playq_notify_done();
}

void
playq_song_fast_movedown(struct vfsref *vr, unsigned int index)
{
	struct vfsref *nvr;

	/* Remove the item below */
	nvr = vfs_list_next(vr);
	gui_playq_notify_pre_removal(index + 1);
	vfs_list_remove(&playq_list, nvr);

	/* Add it above */
	vfs_list_insert_before(&playq_list, nvr, vr);
	gui_playq_notify_post_insertion(index);
	gui_playq_notify_done();
}

void
playq_song_fast_select(struct vfsref *vr, unsigned int index)
{
	if (funcs->select(vr) != 0)
		return;

	/* Now go to the next song */
	playq_flags = (playq_flags & ~PF_PAUSE) | PF_SKIP;
	g_cond_signal(playq_wakeup);
}

void
playq_song_remove_all(void)
{
	struct vfsref *vr;

	PLAYQ_LOCK;
	while ((vr = vfs_list_first(&playq_list)) != NULL) {
		funcs->notify_pre_removal(vr);
		gui_playq_notify_pre_removal(1);
		vfs_list_remove(&playq_list, vr);
		vfs_close(vr);
	}
	gui_playq_notify_done();
	PLAYQ_UNLOCK;
}

void
playq_song_randomize(void)
{
	unsigned int remaining, idx = 0;
	struct vfsref *vr, **vrlist;

	/*
	 * This code implements the Fisher-Yates algorithm to randomize
	 * the playlist. Only difference is that we already place items
	 * back in the list instead of actually swapping them.
	 */

	PLAYQ_LOCK;
	remaining = vfs_list_items(&playq_list);
	if (remaining < 2)
		goto done;

	/* Generate a shadow list */
	vrlist = g_new(struct vfsref *, remaining);
	vfs_list_foreach(&playq_list, vr)
		vrlist[idx++] = vr;
	vfs_list_init(&playq_list);

	for (; remaining != 0; remaining--) {
		/* Pick a random item from the beginning */
		idx = g_random_int_range(0, remaining);
		
		/* Add it to the list */
		vfs_list_insert_tail(&playq_list, vrlist[idx]);
		/* Remove fragmentation  */
		vrlist[idx] = vrlist[remaining - 1];
	}

	/* Trash the shadow list */
	g_free(vrlist);

	gui_playq_notify_post_randomization();
	gui_playq_notify_done();
done:	PLAYQ_UNLOCK;
}
