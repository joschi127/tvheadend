/*
 *  Digital Video Recorder
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include "settings.h"

#include "tvheadend.h"
#include "dvr.h"
#include "htsp_server.h"
#include "streaming.h"
#include "intlconv.h"
#include "dbus.h"
#include "imagecache.h"
#include "access.h"

struct dvr_entry_list dvrentries;

#if ENABLE_DBUS_1
static gtimer_t dvr_dbus_timer;
#endif

static void dvr_entry_destroy(dvr_entry_t *de, int delconf);
static void dvr_timer_expire(void *aux);
static void dvr_timer_start_recording(void *aux);
static void dvr_timer_stop_recording(void *aux);
static int dvr_entry_class_disp_title_set(void *o, const void *v);
static int dvr_entry_class_disp_subtitle_set(void *o, const void *v);

/*
 * Start / stop time calculators
 */
static inline int extra_valid(time_t extra)
{
  return extra != 0 && extra != (time_t)-1;
}

int
dvr_entry_get_start_time( dvr_entry_t *de )
{
  /* Note 30 seconds might not be enough (rotors) */
  return de->de_start - (60 * dvr_entry_get_extra_time_pre(de)) - 30;
}

int
dvr_entry_get_stop_time( dvr_entry_t *de )
{
  return de->de_stop + (60 * dvr_entry_get_extra_time_post(de));
}

int
dvr_entry_get_extra_time_pre( dvr_entry_t *de )
{
  time_t extra = de->de_start_extra;

  if (de->de_timerec)
    return 0;
  if (!extra_valid(extra)) {
    if (de->de_channel)
      extra = de->de_channel->ch_dvr_extra_time_pre;
    if (!extra_valid(extra))
      extra = de->de_config->dvr_extra_time_pre;
  }
  return extra;
}

int
dvr_entry_get_extra_time_post( dvr_entry_t *de )
{
  time_t extra = de->de_stop_extra;

  if (de->de_timerec)
    return 0;
  if (!extra_valid(extra)) {
    if (de->de_channel)
      extra = de->de_channel->ch_dvr_extra_time_post;
    if (!extra_valid(extra))
      extra = de->de_config->dvr_extra_time_post;
  }
  return extra;
}

int
dvr_entry_get_mc( dvr_entry_t *de )
{
  if (de->de_mc >= 0)
    return de->de_mc;
  return profile_get_mc(de->de_config->dvr_profile);
}

int
dvr_entry_get_retention( dvr_entry_t *de )
{
  if (de->de_retention > 0)
    return de->de_retention;
  return de->de_config->dvr_retention_days;
}

/*
 * DBUS next dvr start notifications
 */
#if ENABLE_DBUS_1
static void
dvr_dbus_timer_cb( void *aux )
{
  dvr_entry_t *de;
  time_t result, start, max = 0;
  static time_t last_result = 0;

  lock_assert(&global_lock);

  /* find the maximum value */
  LIST_FOREACH(de, &dvrentries, de_global_link) {
    if (de->de_sched_state != DVR_SCHEDULED)
      continue;
    start = dvr_entry_get_start_time(de);
    if (dispatch_clock < start && start > max)
      max = start;
  }
  /* lower the maximum value */
  result = max;
  LIST_FOREACH(de, &dvrentries, de_global_link) {
    if (de->de_sched_state != DVR_SCHEDULED)
      continue;
    start = dvr_entry_get_start_time(de);
    if (dispatch_clock < start && start < result)
      result = start;
  }
  /* different? send it.... */
  if (result && result != last_result) {
    dbus_emit_signal_s64("/dvr", "next", result);
    last_result = result;
  }
}
#endif

/*
 * Completed
 */
static void
_dvr_entry_completed(dvr_entry_t *de)
{
  de->de_sched_state = DVR_COMPLETED;
#if ENABLE_INOTIFY
  dvr_inotify_add(de);
#endif
}

/**
 * Return printable status for a dvr entry
 */
const char *
dvr_entry_status(dvr_entry_t *de)
{
  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    return "Scheduled for recording";
    
  case DVR_RECORDING:

    switch(de->de_rec_state) {
    case DVR_RS_PENDING:
      return "Waiting for stream";
    case DVR_RS_WAIT_PROGRAM_START:
      return "Waiting for program start";
    case DVR_RS_RUNNING:
      return "Running";
    case DVR_RS_COMMERCIAL:
      return "Commercial break";
    case DVR_RS_ERROR:
      return streaming_code2txt(de->de_last_error);
    default:
      return "Invalid";
    }

  case DVR_COMPLETED:
    if(dvr_get_filesize(de) == -1)
      return "File Missing";
    if(de->de_last_error)
      return streaming_code2txt(de->de_last_error);
    else
      return "Completed OK";

  case DVR_MISSED_TIME:
    return "Time missed";

  default:
    return "Invalid";
  }
}


/**
 *
 */
const char *
dvr_entry_schedstatus(dvr_entry_t *de)
{
  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    return "scheduled";
  case DVR_RECORDING:
    if(de->de_last_error)
      return "recordingError";
    else
      return "recording";
  case DVR_COMPLETED:
    if(de->de_last_error || dvr_get_filesize(de) == -1)
      return "completedError";
    else
      return "completed";
  case DVR_MISSED_TIME:
    return "completedError";
  default:
    return "unknown";
  }
}

/**
 *
 */
void
dvr_make_title(char *output, size_t outlen, dvr_entry_t *de)
{
  struct tm tm;
  char buf[40];
  dvr_config_t *cfg = de->de_config;

  if(cfg->dvr_channel_in_title)
    snprintf(output, outlen, "%s-", DVR_CH_NAME(de));
  else
    output[0] = 0;
  
  if (cfg->dvr_omit_title == 0)
    snprintf(output + strlen(output), outlen - strlen(output),
	     "%s", lang_str_get(de->de_title, NULL));

  if (cfg->dvr_episode_before_date) {
    if (cfg->dvr_episode_in_title && de->de_bcast && de->de_bcast->episode)
      epg_episode_number_format(de->de_bcast->episode,
                                output + strlen(output),
                                outlen - strlen(output),
                                ".", "S%02d", NULL, "E%02d", NULL);
  }

  if (cfg->dvr_subtitle_in_title && de->de_subtitle) {
      snprintf(output + strlen(output), outlen - strlen(output),
           ".%s", lang_str_get(de->de_subtitle, NULL));
  }

  localtime_r(&de->de_start, &tm);
  
  if (cfg->dvr_date_in_title) {
    strftime(buf, sizeof(buf), "%F", &tm);
    snprintf(output + strlen(output), outlen - strlen(output), ".%s", buf);
  }

  if (cfg->dvr_time_in_title) {
    strftime(buf, sizeof(buf), "%H-%M", &tm);
    snprintf(output + strlen(output), outlen - strlen(output), ".%s", buf);
  }

  if (!cfg->dvr_episode_before_date) {
    if(cfg->dvr_episode_in_title) {
      if(de->de_bcast && de->de_bcast->episode)
        epg_episode_number_format(de->de_bcast->episode,
                                  output + strlen(output),
                                  outlen - strlen(output),
                                  ".", "S%02d", NULL, "E%02d", NULL);
    }
  }
}

static void
dvr_entry_set_timer(dvr_entry_t *de)
{
  time_t now, start, stop;

  time(&now);

  start = dvr_entry_get_start_time(de);
  stop  = dvr_entry_get_stop_time(de);

  if(now >= stop || de->de_dont_reschedule) {

    if(de->de_filename == NULL)
      de->de_sched_state = DVR_MISSED_TIME;
    else
      _dvr_entry_completed(de);
    gtimer_arm_abs(&de->de_timer, dvr_timer_expire, de,
                   de->de_stop + dvr_entry_get_retention(de) * 86400);

  } else if (de->de_sched_state == DVR_RECORDING)  {

    gtimer_arm_abs(&de->de_timer, dvr_timer_stop_recording, de, stop);

  } else if (de->de_channel && de->de_channel->ch_enabled) {

    de->de_sched_state = DVR_SCHEDULED;

    tvhtrace("dvr", "entry timer scheduled for %"PRItime_t, start);
    gtimer_arm_abs(&de->de_timer, dvr_timer_start_recording, de, start);
#if ENABLE_DBUS_1
    gtimer_arm(&dvr_dbus_timer, dvr_dbus_timer_cb, NULL, 5);
#endif

  } else {

    de->de_sched_state = DVR_NOSTATE;

  }
}


/**
 * Get episode name
 */
static char *
dvr_entry_get_episode(epg_broadcast_t *bcast, char *buf, int len)
{
  if (!bcast || !bcast->episode)
    return NULL;
  if (epg_episode_number_format(bcast->episode,
                                buf, len, NULL,
                                "Season %d", ".", "Episode %d", "/%d"))
    return buf;
  return NULL;
}

/**
 * Find dvr entry using 'fuzzy' search
 */
static int
dvr_entry_fuzzy_match(dvr_entry_t *de, epg_broadcast_t *e)
{
  time_t t1, t2;
  const char *title1, *title2;
  char buf[64];

  /* Matching ID */
  if (de->de_dvb_eid && de->de_dvb_eid == e->dvb_eid)
    return 1;

  /* No title */
  if (!(title1 = epg_broadcast_get_title(e, NULL)))
    return 0;
  if (!(title2 = lang_str_get(de->de_title, NULL)))
    return 0;

  /* Wrong length (+/-20%) */
  t1 = de->de_stop - de->de_start;
  t2  = e->stop - e->start;
  if ( abs(t2 - t1) > (t1 / 5) )
    return 0;

  /* Outside of window */
  if (abs(e->start - de->de_start) > de->de_config->dvr_update_window)
    return 0;
  
  /* Title match (or contains?) */
  if (strcmp(title1, title2))
    return 0;

  /* episode check */
  if (dvr_entry_get_episode(e, buf, sizeof(buf)) && de->de_episode)
    if (strcmp(buf, de->de_episode))
      return 0;

  return 1;
}

/**
 * Create the event from config
 */
dvr_entry_t *
dvr_entry_create(const char *uuid, htsmsg_t *conf)
{
  dvr_entry_t *de, *de2;
  int64_t start, stop;
  const char *s;

  if (conf) {
    if (htsmsg_get_s64(conf, "start", &start))
      return NULL;
    if (htsmsg_get_s64(conf, "stop", &stop))
      return NULL;
    if ((htsmsg_get_str(conf, "channel")) == NULL &&
        (htsmsg_get_str(conf, "channelname")) == NULL)
      return NULL;
  }

  de = calloc(1, sizeof(*de));

  if (idnode_insert(&de->de_id, uuid, &dvr_entry_class, IDNODE_SHORT_UUID)) {
    if (uuid)
      tvhwarn("dvr", "invalid entry uuid '%s'", uuid);
    free(de);
    return NULL;
  }

  de->de_mc = -1;
  de->de_config = dvr_config_find_by_name_default(NULL);
  if (de->de_config)
    LIST_INSERT_HEAD(&de->de_config->dvr_entries, de, de_config_link);

  idnode_load(&de->de_id, conf);

  /* special case, becaous PO_NOSAVE, load ignores it */
  if (de->de_title == NULL &&
      (s = htsmsg_get_str(conf, "disp_title")) != NULL)
    dvr_entry_class_disp_title_set(de, s);

  /* special case, becaous PO_NOSAVE, load ignores it */
  if (de->de_subtitle == NULL &&
          (s = htsmsg_get_str(conf, "disp_subtitle")) != NULL)
    dvr_entry_class_disp_subtitle_set(de, s);

  de->de_refcnt = 1;

  LIST_INSERT_HEAD(&dvrentries, de, de_global_link);

  if (de->de_channel) {
    LIST_FOREACH(de2, &de->de_channel->ch_dvrs, de_channel_link)
      if(de2 != de &&
         de2->de_start == de->de_start &&
         de2->de_sched_state != DVR_COMPLETED) {
        dvr_entry_destroy(de, 0);
        return NULL;
      }
  }

  dvr_entry_set_timer(de);
  htsp_dvr_entry_add(de);

  return de;
}

/**
 * Create the event
 */
dvr_entry_t *
dvr_entry_create_(const char *config_uuid, epg_broadcast_t *e,
                  channel_t *ch, time_t start, time_t stop,
                  time_t start_extra, time_t stop_extra,
                  const char *title, const char* subtitle, const char *description,
                  const char *lang, epg_genre_t *content_type,
                  const char *owner,
                  const char *creator, dvr_autorec_entry_t *dae,
                  dvr_timerec_entry_t *dte,
                  dvr_prio_t pri, int retention,
                  const char *comment)
{
  dvr_entry_t *de;
  char tbuf[64], *s;
  struct tm tm;
  time_t t;
  lang_str_t *l;
  htsmsg_t *conf;

  conf = htsmsg_create_map();
  htsmsg_add_s64(conf, "start", start);
  htsmsg_add_s64(conf, "stop", stop);
  htsmsg_add_str(conf, "channel", idnode_uuid_as_str(&ch->ch_id));
  htsmsg_add_u32(conf, "pri", pri);
  htsmsg_add_u32(conf, "retention", retention);
  htsmsg_add_str(conf, "config_name", config_uuid ?: "");
  htsmsg_add_s64(conf, "start_extra", start_extra);
  htsmsg_add_s64(conf, "stop_extra", stop_extra);
  htsmsg_add_str(conf, "owner", owner ?: "");
  htsmsg_add_str(conf, "creator", creator ?: "");
  htsmsg_add_str(conf, "comment", comment ?: "");
  if (e) {
    htsmsg_add_u32(conf, "dvb_eid", e->dvb_eid);
    if (e->episode && e->episode->title)
      lang_str_serialize(e->episode->title, conf, "title");
    if (e->episode && e->episode->subtitle)
      lang_str_serialize(e->episode->subtitle, conf, "subtitle");
    if (e->description)
      lang_str_serialize(e->description, conf, "description");
    else if (e->episode && e->episode->description)
      lang_str_serialize(e->episode->description, conf, "description");
    else if (e->summary)
      lang_str_serialize(e->summary, conf, "description");
    else if (e->episode && e->episode->summary)
      lang_str_serialize(e->episode->summary, conf, "description");
    if (e->episode && (s = dvr_entry_get_episode(e, tbuf, sizeof(tbuf))))
      htsmsg_add_str(conf, "episode", s);
  } else if (title) {
    l = lang_str_create();
    lang_str_add(l, title, lang, 0);
    lang_str_serialize(l, conf, "title");
    lang_str_destroy(l);
    if (description) {
      l = lang_str_create();
      lang_str_add(l, description, lang, 0);
      lang_str_serialize(l, conf, "description");
      lang_str_destroy(l);
    }
    if (subtitle) {
      l = lang_str_create();
      lang_str_add(l, subtitle, lang, 0);
      lang_str_serialize(l, conf, "subtitle");
      lang_str_destroy(l);
    }
  }
  if (content_type)
    htsmsg_add_u32(conf, "content_type", content_type->code / 16);
  if (e)
    htsmsg_add_u32(conf, "broadcast", e->id);
  if (dae)
  {
    htsmsg_add_str(conf, "autorec", idnode_uuid_as_str(&dae->dae_id));
    htsmsg_add_str(conf, "directory", dae->dae_directory ?: "");
  }
  if (dte)
  {
    htsmsg_add_str(conf, "timerec", idnode_uuid_as_str(&dte->dte_id));
    htsmsg_add_str(conf, "directory", dte->dte_directory ?: "");
  }

  de = dvr_entry_create(NULL, conf);

  htsmsg_destroy(conf);

  if (de == NULL)
    return NULL;

  t = dvr_entry_get_start_time(de);
  localtime_r(&t, &tm);
  if (strftime(tbuf, sizeof(tbuf), "%F %T", &tm) <= 0)
    *tbuf = 0;

  tvhlog(LOG_INFO, "dvr", "entry %s \"%s\" on \"%s\" starting at %s, "
	 "scheduled for recording by \"%s\"",
         idnode_uuid_as_str(&de->de_id),
	 lang_str_get(de->de_title, NULL), DVR_CH_NAME(de), tbuf, creator);

  dvr_entry_save(de);
  return de;
}

/**
 *
 */
dvr_entry_t *
dvr_entry_create_htsp(const char *config_uuid,
                      channel_t *ch, time_t start, time_t stop,
                      time_t start_extra, time_t stop_extra,
                      const char *title, const char* subtitle,
                      const char *description, const char *lang,
                      epg_genre_t *content_type,
                      const char *owner,
                      const char *creator, dvr_autorec_entry_t *dae,
                      dvr_prio_t pri, int retention,
                      const char *comment)
{
  dvr_config_t *cfg = dvr_config_find_by_uuid(config_uuid);
  if (!cfg)
    cfg = dvr_config_find_by_name(config_uuid);
  return dvr_entry_create_(cfg ? idnode_uuid_as_str(&cfg->dvr_id) : NULL,
                           NULL,
                           ch, start, stop, start_extra, stop_extra,
                           title, subtitle, description, lang, content_type,
                           owner, creator, dae, NULL, pri, retention,
                           comment);
}

/**
 *
 */
dvr_entry_t *
dvr_entry_create_by_event(const char *config_uuid,
                          epg_broadcast_t *e,
                          time_t start_extra, time_t stop_extra,
                          const char *owner,
                          const char *creator, dvr_autorec_entry_t *dae,
                          dvr_prio_t pri, int retention,
                          const char *comment)
{
  if(!e->channel || !e->episode || !e->episode->title)
    return NULL;

  return dvr_entry_create_(config_uuid, e,
                           e->channel, e->start, e->stop,
                           start_extra, stop_extra,
                           NULL, NULL, NULL, NULL,
                           LIST_FIRST(&e->episode->genre),
                           owner, creator, dae, NULL, pri, retention,
                           comment);
}

/**
 *
 */
static dvr_entry_t* _dvr_duplicate_event(dvr_entry_t* de)
{
  if (!de->de_autorec)
    return NULL;

  int record = de->de_autorec->dae_record;

  struct tm de_start;
  localtime_r(&de->de_start, &de_start);

  switch (record) {
    case DVR_AUTOREC_RECORD_ALL:
      return NULL;
    case DVR_AUTOREC_RECORD_DIFFERENT_EPISODE_NUMBER:
      if (strempty(de->de_episode))
        return NULL;
      break;
    case DVR_AUTOREC_RECORD_DIFFERENT_SUBTITLE:
      if (lang_str_empty(de->de_subtitle))
        return NULL;
      break;
    case DVR_AUTOREC_RECORD_DIFFERENT_DESCRIPTION:
      if (lang_str_empty(de->de_desc))
        return NULL;
      break;
    case DVR_AUTOREC_RECORD_ONCE_PER_WEEK:
      de_start.tm_mday -= (de_start.tm_wday + 6) % 7; // week = mon-sun
      mktime(&de_start); // adjusts de_start
      break;
  }

  // title not defined, can't be deduped
  if (lang_str_empty(de->de_title))
    return NULL;

  dvr_entry_t *de2;

  LIST_FOREACH(de2, &dvrentries, de_global_link) {
    if (de == de2)
      continue;

    // only earlier recordings qualify as master
    if (de2->de_start > de->de_start)
      continue;

    // only successful earlier recordings qualify as master
    if (de2->de_sched_state == DVR_MISSED_TIME || (de2->de_sched_state == DVR_COMPLETED && de2->de_last_error != SM_CODE_OK))
      continue;

    // if titles are not defined or do not match, don't dedup
    if (lang_str_compare(de->de_title, de2->de_title))
      continue;

    switch (record) {
      case DVR_AUTOREC_RECORD_DIFFERENT_EPISODE_NUMBER:
        if (!strcmp(de->de_episode, de2->de_episode))
          return de2;
        break;
      case DVR_AUTOREC_RECORD_DIFFERENT_SUBTITLE:
        if (!lang_str_compare(de->de_subtitle, de2->de_subtitle))
          return de2;
        break;
      case DVR_AUTOREC_RECORD_DIFFERENT_DESCRIPTION:
        if (!lang_str_compare(de->de_desc, de2->de_desc))
          return de2;
        break;
      case DVR_AUTOREC_RECORD_ONCE_PER_WEEK: {
        struct tm de2_start;
        localtime_r(&de2->de_start, &de2_start);
        de2_start.tm_mday -= (de2_start.tm_wday + 6) % 7; // week = mon-sun
        mktime(&de2_start); // adjusts de2_start
        if (de_start.tm_year == de2_start.tm_year && de_start.tm_yday == de2_start.tm_yday)
          return de2;
        break;
      }
      case DVR_AUTOREC_RECORD_ONCE_PER_DAY: {
        struct tm de2_start;
        localtime_r(&de2->de_start, &de2_start);
        if (de_start.tm_year == de2_start.tm_year && de_start.tm_yday == de2_start.tm_yday)
          return de2;
        break;
      }
    }
  }
  return NULL;
}

/**
 *
 */
void
dvr_entry_create_by_autorec(epg_broadcast_t *e, dvr_autorec_entry_t *dae)
{
  char buf[200];

  /* Identical duplicate detection
     NOTE: Semantic duplicate detection is deferred to the start time of recording and then done using _dvr_duplicate_event by dvr_timer_start_recording. */
  dvr_entry_t* de;
  LIST_FOREACH(de, &dvrentries, de_global_link) {
    if (de->de_bcast == e || (de->de_bcast && de->de_bcast->episode == e->episode))
      return;
  }

  snprintf(buf, sizeof(buf), "Auto recording%s%s",
           dae->dae_creator ? " by: " : "",
           dae->dae_creator ?: "");

  dvr_entry_create_by_event(idnode_uuid_as_str(&dae->dae_config->dvr_id), e,
                            dae->dae_start_extra, dae->dae_stop_extra,
                            dae->dae_owner, buf, dae, dae->dae_pri, dae->dae_retention,
                            dae->dae_comment);
}

/**
 *
 */
void
dvr_entry_dec_ref(dvr_entry_t *de)
{
  lock_assert(&global_lock);

  if(de->de_refcnt > 1) {
    de->de_refcnt--;
    return;
  }

  idnode_unlink(&de->de_id);

  if(de->de_autorec != NULL)
    LIST_REMOVE(de, de_autorec_link);

  if (de->de_timerec) {
    de->de_timerec->dte_spawn = NULL;
    de->de_timerec = NULL;
  }

  if(de->de_config != NULL)
    LIST_REMOVE(de, de_config_link);

  free(de->de_filename);
  free(de->de_owner);
  free(de->de_creator);
  free(de->de_comment);
  if (de->de_title) lang_str_destroy(de->de_title);
  if (de->de_subtitle)  lang_str_destroy(de->de_subtitle);
  if (de->de_desc)  lang_str_destroy(de->de_desc);
  if (de->de_bcast) de->de_bcast->putref((epg_object_t*)de->de_bcast);
  free(de->de_channel_name);
  free(de->de_episode);

  free(de);
}

/**
 *
 */
static void
dvr_entry_destroy(dvr_entry_t *de, int delconf)
{
  if (delconf)
    hts_settings_remove("dvr/log/%s", idnode_uuid_as_str(&de->de_id));

  htsp_dvr_entry_delete(de);
  
#if ENABLE_INOTIFY
  dvr_inotify_del(de);
#endif

  gtimer_disarm(&de->de_timer);
#if ENABLE_DBUS_1
  gtimer_arm(&dvr_dbus_timer, dvr_dbus_timer_cb, NULL, 2);
#endif

  if (de->de_channel)
    LIST_REMOVE(de, de_channel_link);
  LIST_REMOVE(de, de_global_link);
  de->de_channel = NULL;

  dvr_entry_dec_ref(de);
}

/**
 *
 */
void
dvr_entry_destroy_by_config(dvr_config_t *cfg, int delconf)
{
  dvr_entry_t *de;
  dvr_config_t *def = NULL;

  while ((de = LIST_FIRST(&cfg->dvr_entries)) != NULL) {
    LIST_REMOVE(de, de_config_link);
    if (def == NULL && delconf)
      def = dvr_config_find_by_name_default(NULL);
    de->de_config = def;
    if (def)
      LIST_INSERT_HEAD(&def->dvr_entries, de, de_config_link);
    if (delconf)
      dvr_entry_save(de);
  }
}

/**
 *
 */
void
dvr_entry_save(dvr_entry_t *de)
{
  htsmsg_t *m = htsmsg_create_map();

  lock_assert(&global_lock);

  idnode_save(&de->de_id, m);
  hts_settings_save(m, "dvr/log/%s", idnode_uuid_as_str(&de->de_id));
  htsmsg_destroy(m);
}


/**
 *
 */
static void
dvr_timer_expire(void *aux)
{
  dvr_entry_t *de = aux;
  dvr_entry_destroy(de, 1);
 
}

static dvr_entry_t *_dvr_entry_update
  ( dvr_entry_t *de, epg_broadcast_t *e, const char *title, const char* subtitle,
    const char *desc, const char *lang, time_t start, time_t stop,
    time_t start_extra, time_t stop_extra,  dvr_prio_t pri, int retention )
{
  char buf[40];
  int save = 0;

  if (!dvr_entry_is_editable(de))
    return de;

  /* Start/Stop */
  if (e) {
    start = e->start;
    stop  = e->stop;
  }
  if (start && (start != de->de_start)) {
    de->de_start = start;
    save = 1;
  }
  if (stop && (stop != de->de_stop)) {
    de->de_stop = stop;
    save = 1;
  }
  if (start_extra && (start_extra != de->de_start_extra)) {
    de->de_start_extra = start_extra;
    save = 1;
  }
  if (stop_extra && (stop_extra != de->de_stop_extra)) {
    de->de_stop_extra = stop_extra;
    save = 1;
  }
  if (pri != DVR_PRIO_NOTSET && (pri != de->de_pri)) {
    de->de_pri = pri;
    save = 1;
  }
  if (retention && (retention != de->de_retention)) {
    de->de_retention = retention;
    save = 1;
  }
  if (save)
    dvr_entry_set_timer(de);

  /* Title */ 
  if (e && e->episode && e->episode->title) {
    if (de->de_title) lang_str_destroy(de->de_title);
    de->de_title = lang_str_copy(e->episode->title);
  } else if (title) {
    if (!de->de_title) de->de_title = lang_str_create();
    save = lang_str_add(de->de_title, title, lang, 1);
  }

  /* Subtitle*/
  if (subtitle) {
    if (!de->de_subtitle) de->de_subtitle = lang_str_create();
    save = lang_str_add(de->de_subtitle, subtitle, lang, 1);
  }

  /* EID */
  if (e && e->dvb_eid != de->de_dvb_eid) {
    de->de_dvb_eid = e->dvb_eid;
    save = 1;
  }

  // TODO: description

  /* Genre */
  if (e && e->episode) {
    epg_genre_t *g = LIST_FIRST(&e->episode->genre);
    if (g && (g->code / 16) != de->de_content_type) {
      de->de_content_type = g->code / 16;
      save = 1;
    }
  }

  /* Broadcast */
  if (e && (de->de_bcast != e)) {
    if (de->de_bcast)
      de->de_bcast->putref(de->de_bcast);
    de->de_bcast = e;
    e->getref(e);
    save = 1;

  }

  /* Episode */
  if (dvr_entry_get_episode(de->de_bcast, buf, sizeof(buf))) {
    if (strcmp(de->de_episode ?: "", buf)) {
      free(de->de_episode);
      de->de_episode = strdup(buf);
      save = 1;
    }
  }

  /* Save changes */
  if (save) {
    idnode_changed(&de->de_id);
    htsp_dvr_entry_update(de);
    tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\": Updated Timer",
           lang_str_get(de->de_title, NULL), DVR_CH_NAME(de));
  }

  return de;
}

/**
 *
 */
dvr_entry_t * 
dvr_entry_update
  (dvr_entry_t *de,
   const char* de_title, const char* de_subtitle, const char *de_desc, const char *lang,
   time_t de_start, time_t de_stop,
   time_t de_start_extra, time_t de_stop_extra,
   dvr_prio_t pri, int retention)
{
  return _dvr_entry_update(de, NULL, de_title, de_subtitle, de_desc, lang,
                           de_start, de_stop, de_start_extra, de_stop_extra,
                           pri, retention);
}

/**
 * Used to notify the DVR that an event has been replaced in the EPG
 */
void 
dvr_event_replaced(epg_broadcast_t *e, epg_broadcast_t *new_e)
{
  dvr_entry_t *de;
  assert(e != NULL);
  assert(new_e != NULL);

  /* Ignore */
  if ( e == new_e ) return;

  /* Existing entry */
  if ((de = dvr_entry_find_by_event(e))) {
    tvhtrace("dvr",
             "dvr entry %s event replaced %s on %s @ %"PRItime_t
             " to %"PRItime_t,
             idnode_uuid_as_str(&de->de_id),
             epg_broadcast_get_title(e, NULL),
             channel_get_name(e->channel),
             e->start, e->stop);

    /* Ignore - already in progress */
    if (de->de_sched_state != DVR_SCHEDULED)
      return;

    /* Unlink the broadcast */
    e->putref(e);
    de->de_bcast = NULL;

    /* If this was created by autorec - just remove it, it'll get recreated */
    if (de->de_autorec) {
      dvr_entry_destroy(de, 1);

    /* Find match */
    } else {
      RB_FOREACH(e, &e->channel->ch_epg_schedule, sched_link) {
        if (dvr_entry_fuzzy_match(de, e)) {
          tvhtrace("dvr",
                   "  replacement event %s on %s @ %"PRItime_t
                   " to %"PRItime_t,
                   epg_broadcast_get_title(e, NULL),
                   channel_get_name(e->channel),
                   e->start, e->stop);
          e->getref(e);
          de->de_bcast = e;
          _dvr_entry_update(de, e, NULL, NULL, NULL, NULL, 0, 0, 0, 0, DVR_PRIO_NOTSET, 0);
          break;
        }
      }
    }
  }
}

void dvr_event_updated ( epg_broadcast_t *e )
{
  dvr_entry_t *de;
  de = dvr_entry_find_by_event(e);
  if (de)
    _dvr_entry_update(de, e, NULL, NULL, NULL, NULL, 0, 0, 0, 0, DVR_PRIO_NOTSET, 0);
  else {
    LIST_FOREACH(de, &dvrentries, de_global_link) {
      if (de->de_sched_state != DVR_SCHEDULED) continue;
      if (de->de_bcast) continue;
      if (de->de_channel != e->channel) continue;
      if (dvr_entry_fuzzy_match(de, e)) {
        tvhtrace("dvr",
                 "dvr entry %s link to event %s on %s @ %"PRItime_t
                 " to %"PRItime_t,
                 idnode_uuid_as_str(&de->de_id),
                 epg_broadcast_get_title(e, NULL),
                 channel_get_name(e->channel),
                 e->start, e->stop);
        e->getref(e);
        de->de_bcast = e;
        _dvr_entry_update(de, e, NULL, NULL, NULL, NULL, 0, 0, 0, 0, DVR_PRIO_NOTSET, 0);
        break;
      }
    }
  }
}

/**
 *
 */
static void
dvr_stop_recording(dvr_entry_t *de, int stopcode, int saveconf)
{
  if (de->de_rec_state == DVR_RS_PENDING ||
      de->de_rec_state == DVR_RS_WAIT_PROGRAM_START ||
      de->de_filename == NULL)
    de->de_sched_state = DVR_MISSED_TIME;
  else
    _dvr_entry_completed(de);

  dvr_rec_unsubscribe(de, stopcode);

  tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\": "
	 "End of program: %s",
	 lang_str_get(de->de_title, NULL), DVR_CH_NAME(de),
	 dvr_entry_status(de));

  if (saveconf)
    dvr_entry_save(de);
  idnode_notify_simple(&de->de_id);
  htsp_dvr_entry_update(de);

  gtimer_arm_abs(&de->de_timer, dvr_timer_expire, de, 
		 de->de_stop + dvr_entry_get_retention(de) * 86400);
}


/**
 *
 */
static void
dvr_timer_stop_recording(void *aux)
{
  dvr_stop_recording(aux, 0, 1);
}



/**
 *
 */
static void
dvr_timer_start_recording(void *aux)
{
  dvr_entry_t *de = aux;

  if (de->de_channel == NULL || !de->de_channel->ch_enabled) {
    de->de_sched_state = DVR_NOSTATE;
    return;
  }

  // if duplicate, then delete it now, don't record!
  if (_dvr_duplicate_event(de)) {
    dvr_entry_cancel_delete(de);
    return;
  }

  de->de_sched_state = DVR_RECORDING;
  de->de_rec_state = DVR_RS_PENDING;

  tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\" recorder starting",
	 lang_str_get(de->de_title, NULL), DVR_CH_NAME(de));

  idnode_changed(&de->de_id);
  htsp_dvr_entry_update(de);
  dvr_rec_subscribe(de);

  gtimer_arm_abs(&de->de_timer, dvr_timer_stop_recording, de, 
                 dvr_entry_get_stop_time(de));
}


/**
 *
 */
dvr_entry_t *
dvr_entry_find_by_id(int id)
{
  dvr_entry_t *de;
  LIST_FOREACH(de, &dvrentries, de_global_link)
    if(idnode_get_short_uuid(&de->de_id) == id)
      break;
  return de;  
}


/**
 *
 */
dvr_entry_t *
dvr_entry_find_by_event(epg_broadcast_t *e)
{
  dvr_entry_t *de;

  if(!e->channel) return NULL;

  LIST_FOREACH(de, &e->channel->ch_dvrs, de_channel_link)
    if(de->de_bcast == e) return de;
  return NULL;
}

/*
 * Find DVR entry based on an episode
 */
dvr_entry_t *
dvr_entry_find_by_episode(epg_broadcast_t *e)
{
  if (e->episode) {
    dvr_entry_t *de;
    epg_broadcast_t *ebc;
    LIST_FOREACH(ebc, &e->episode->broadcasts, ep_link) {
      de = dvr_entry_find_by_event(ebc);
      if (de) return de;
    }
    return NULL;
  } else {
    return dvr_entry_find_by_event(e);
  }
}

/**
 * Unconditionally remove an entry
 */
static void
dvr_entry_purge(dvr_entry_t *de, int delconf)
{
  if(de->de_sched_state == DVR_RECORDING)
    dvr_stop_recording(de, SM_CODE_SOURCE_DELETED, delconf);
}


/* **************************************************************************
 * DVR Entry Class definition
 * **************************************************************************/

static void
dvr_entry_class_save(idnode_t *self)
{
  dvr_entry_t *de = (dvr_entry_t *)self;
  dvr_entry_save(de);
  if (dvr_entry_is_valid(de))
    dvr_entry_set_timer(de);
}

static void
dvr_entry_class_delete(idnode_t *self)
{
  dvr_entry_cancel_delete((dvr_entry_t *)self);
}

static int
dvr_entry_class_perm(idnode_t *self, access_t *a, htsmsg_t *msg_to_write)
{
  dvr_entry_t *de = (dvr_entry_t *)self;

  if (access_verify2(a, ACCESS_OR|ACCESS_ADMIN|ACCESS_RECORDER))
    return -1;
  if (!access_verify2(a, ACCESS_ADMIN))
    return 0;
  if (dvr_entry_verify(de, a, msg_to_write == NULL ? 1 : 0))
    return -1;
  return 0;
}

static const char *
dvr_entry_class_get_title (idnode_t *self)
{
  dvr_entry_t *de = (dvr_entry_t *)self;
  const char *s;
  s = lang_str_get(de->de_title, NULL);
  if (s == NULL || s[0] == '\0')
    s = lang_str_get(de->de_desc, NULL);
  return s;
}

static int
dvr_entry_class_time_set(dvr_entry_t *de, time_t *v, time_t nv)
{
  if (!dvr_entry_is_editable(de))
    return 0;
  if (nv != *v) {
    *v = nv;
    return 1;
  }
  return 0;
}

static int
dvr_entry_class_int_set(dvr_entry_t *de, int *v, int nv)
{
  if (!dvr_entry_is_editable(de))
    return 0;
  if (nv != *v) {
    *v = nv;
    return 1;
  }
  return 0;
}

static int
dvr_entry_class_start_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  return dvr_entry_class_time_set(de, &de->de_start, *(time_t *)v);
}

static uint32_t
dvr_entry_class_start_opts(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de && !dvr_entry_is_editable(de))
    return PO_RDONLY;
  return 0;
}

static uint32_t
dvr_entry_class_start_extra_opts(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de && !dvr_entry_is_editable(de))
    return PO_RDONLY | PO_DURATION;
  return PO_DURATION;
}

static int
dvr_entry_class_start_extra_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  return dvr_entry_class_time_set(de, &de->de_start_extra, *(time_t *)v);
}

static int
dvr_entry_class_stop_set(void *o, const void *_v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  time_t v = *(time_t *)_v;

  if (!dvr_entry_is_editable(de)) {
    if (v < dispatch_clock)
      v = dispatch_clock;
  }
  if (v < de->de_start)
    v = de->de_start;
  return dvr_entry_class_time_set(de, &de->de_stop, v);
}

static int
dvr_entry_class_config_name_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  dvr_config_t *cfg;

  if (!dvr_entry_is_editable(de))
    return 0;
  cfg = v ? dvr_config_find_by_uuid(v) : NULL;
  if (!cfg)
    cfg = dvr_config_find_by_name_default(v);
  if (cfg == NULL) {
    if (de->de_config) {
      LIST_REMOVE(de, de_config_link);
      de->de_config = NULL;
      return 1;
    }
  } else if (de->de_config != cfg) {
    if (de->de_config)
      LIST_REMOVE(de, de_config_link);
    de->de_config = cfg;
    LIST_INSERT_HEAD(&cfg->dvr_entries, de, de_config_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_config_name_get(void *o)
{
  static const char *ret;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_config)
    ret = idnode_uuid_as_str(&de->de_config->dvr_id);
  else
    ret = "";
  return &ret;
}

htsmsg_t *
dvr_entry_class_config_name_list(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_t *p = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "idnode/load");
  htsmsg_add_str(m, "event", "dvrconfig");
  htsmsg_add_u32(p, "enum",  1);
  htsmsg_add_str(p, "class", dvr_config_class.ic_class);
  htsmsg_add_msg(m, "params", p);
  return m;
}

static char *
dvr_entry_class_config_name_rend(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_config)
    return strdup(de->de_config->dvr_config_name);
  return NULL;
}

static int
dvr_entry_class_channel_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  channel_t   *ch;

  if (!dvr_entry_is_editable(de))
    return 0;
  ch = v ? channel_find_by_uuid(v) : NULL;
  if (ch == NULL) {
    if (de->de_channel) {
      LIST_REMOVE(de, de_channel_link);
      free(de->de_channel_name);
      de->de_channel_name = NULL;
      de->de_channel = NULL;
      return 1;
    }
  } else if (de->de_channel != ch) {
    if (de->de_channel)
      LIST_REMOVE(de, de_channel_link);
    free(de->de_channel_name);
    de->de_channel_name = strdup(channel_get_name(ch));
    de->de_channel = ch;
    LIST_INSERT_HEAD(&ch->ch_dvrs, de, de_channel_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_channel_get(void *o)
{
  static const char *ret;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_channel)
    ret = idnode_uuid_as_str(&de->de_channel->ch_id);
  else
    ret = "";
  return &ret;
}

static char *
dvr_entry_class_channel_rend(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_channel)
    return strdup(channel_get_name(de->de_channel));
  return NULL;
}

static int
dvr_entry_class_channel_name_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  channel_t   *ch;
  if (!dvr_entry_is_editable(de))
    return 0;
  if (!strcmp(de->de_channel_name ?: "", v ?: ""))
    return 0;
  ch = v ? channel_find_by_name(v) : NULL;
  if (ch) {
    return dvr_entry_class_channel_set(o, idnode_uuid_as_str(&ch->ch_id));
  } else {
    free(de->de_channel_name);
    de->de_channel_name = v ? strdup(v) : NULL;
    return 1;
  }
}

static const void *
dvr_entry_class_channel_name_get(void *o)
{
  static const char *ret;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_channel)
    ret = channel_get_name(de->de_channel);
  else
    ret = de->de_channel_name;
  return &ret;
}

static int
dvr_entry_class_pri_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  return dvr_entry_class_int_set(de, &de->de_pri, *(int *)v);
}

htsmsg_t *
dvr_entry_class_pri_list ( void *o )
{
  static const struct strtab tab[] = {
    { "Not set",                  DVR_PRIO_NOTSET },
    { "Important",                DVR_PRIO_IMPORTANT },
    { "High",                     DVR_PRIO_HIGH, },
    { "Normal",                   DVR_PRIO_NORMAL },
    { "Low",                      DVR_PRIO_LOW },
    { "Unimportant",              DVR_PRIO_UNIMPORTANT },
  };
  return strtab2htsmsg(tab);
}

static int
dvr_entry_class_retention_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  return dvr_entry_class_int_set(de, &de->de_retention, *(int *)v);
}

static int
dvr_entry_class_mc_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  return dvr_entry_class_int_set(de, &de->de_mc, *(int *)v);
}

htsmsg_t *
dvr_entry_class_mc_list ( void *o )
{
  static const struct strtab tab[] = {
    { "Not set",                       -1 },
    { "Matroska (mkv)",                MC_MATROSKA, },
    { "Same as source (pass through)", MC_PASS, },
#if ENABLE_LIBAV
    { "MPEG-TS",                       MC_MPEGTS },
    { "MPEG-PS (DVD)",                 MC_MPEGPS },
#endif
  };
  return strtab2htsmsg(tab);
}

static int
dvr_entry_class_autorec_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  dvr_autorec_entry_t *dae;
  if (!dvr_entry_is_editable(de))
    return 0;
  dae = v ? dvr_autorec_find_by_uuid(v) : NULL;
  if (dae == NULL) {
    if (de->de_autorec) {
      LIST_REMOVE(de, de_autorec_link);
      de->de_autorec = NULL;
      return 1;
    }
  } else if (de->de_autorec != dae) {
    de->de_autorec = dae;
    LIST_INSERT_HEAD(&dae->dae_spawns, de, de_autorec_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_autorec_get(void *o)
{
  static const char *ret;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_autorec)
    ret = idnode_uuid_as_str(&de->de_autorec->dae_id);
  else
    ret = "";
  return &ret;
}

static int
dvr_entry_class_timerec_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  dvr_timerec_entry_t *dte;
  if (!dvr_entry_is_editable(de))
    return 0;
  dte = v ? dvr_timerec_find_by_uuid(v) : NULL;
  if (dte == NULL) {
    if (de->de_timerec) {
      de->de_timerec->dte_spawn = NULL;
      de->de_timerec = NULL;
      return 1;
    }
  } else if (de->de_timerec != dte) {
    de->de_timerec = dte;
    dte->dte_spawn = de;
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_timerec_get(void *o)
{
  static const char *ret;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_timerec)
    ret = idnode_uuid_as_str(&de->de_timerec->dte_id);
  else
    ret = "";
  return &ret;
}

static int
dvr_entry_class_broadcast_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  uint32_t id = *(uint32_t *)v;
  epg_broadcast_t *bcast;
  if (!dvr_entry_is_editable(de))
    return 0;
  bcast = epg_broadcast_find_by_id(id);
  if (bcast == NULL) {
    if (de->de_bcast) {
      de->de_bcast->putref((epg_object_t*)de->de_bcast);
      de->de_bcast = NULL;
      return 1;
    }
  } else if (de->de_bcast != bcast) {
    if (de->de_bcast)
      de->de_bcast->putref((epg_object_t*)de->de_bcast);
    de->de_bcast = bcast;
    de->de_bcast->getref((epg_object_t*)bcast);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_broadcast_get(void *o)
{
  static uint32_t id;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_bcast)
    id = de->de_bcast->id;
  else
    id = 0;
  return &id;
}

static int
dvr_entry_class_disp_title_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  const char *s = "";
  if (v == NULL || *((char *)v) == '\0')
    v = "UnknownTitle";
  if (de->de_title)
    s = lang_str_get(de->de_title, NULL);
  if (strcmp(s, v)) {
    lang_str_destroy(de->de_title);
    de->de_title = lang_str_create();
    lang_str_add(de->de_title, v, NULL, 0);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_disp_title_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  s = "";
  if (de->de_title) {
    s = lang_str_get(de->de_title, NULL);
    if (s == NULL)
      s = "";
  }
  return &s;
}

static int
dvr_entry_class_disp_subtitle_set(void *o, const void *v)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  const char *s = "";
  if (v == NULL || *((char *)v) == '\0')
    v = "UnknownSubtitle";
  if (de->de_subtitle)
    s = lang_str_get(de->de_subtitle, NULL);
  if (strcmp(s, v)) {
    lang_str_destroy(de->de_subtitle);
    de->de_subtitle = lang_str_create();
    lang_str_add(de->de_subtitle, v, NULL, 0);
    return 1;
  }
  return 0;
}

static const void *
dvr_entry_class_disp_subtitle_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  s = "";
  if (de->de_subtitle) {
    s = lang_str_get(de->de_subtitle, NULL);
    if (s == NULL)
      s = "";
  }
  return &s;
}

static const void *
dvr_entry_class_disp_description_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  s = "";
  if (de->de_desc) {
    s = lang_str_get(de->de_desc, NULL);
    if (s == NULL)
      s = "";
  }
  return &s;
}

static const void *
dvr_entry_class_url_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  static char buf[100];
  s = "";
  if (de->de_sched_state == DVR_COMPLETED ||
      de->de_sched_state == DVR_RECORDING) {
    snprintf(buf, sizeof(buf), "dvrfile/%s", idnode_uuid_as_str(&de->de_id));
    s = buf;
  }
  return &s;
}

static const void *
dvr_entry_class_filesize_get(void *o)
{
  static int64_t size;
  dvr_entry_t *de = (dvr_entry_t *)o;
  if (de->de_sched_state == DVR_COMPLETED ||
      de->de_sched_state == DVR_RECORDING) {
    size = dvr_get_filesize(de);
    if (size < 0)
      size = 0;
  } else
    size = 0;
  return &size;
}

static const void *
dvr_entry_class_start_real_get(void *o)
{
  static time_t tm;
  dvr_entry_t *de = (dvr_entry_t *)o;
  tm = dvr_entry_get_start_time(de);
  return &tm;
}

static const void *
dvr_entry_class_stop_real_get(void *o)
{
  static time_t tm;
  dvr_entry_t *de = (dvr_entry_t *)o;
  tm  = dvr_entry_get_stop_time(de);
  return &tm;
}

static const void *
dvr_entry_class_duration_get(void *o)
{
  static time_t tm;
  time_t start, stop;
  dvr_entry_t *de = (dvr_entry_t *)o;
  start = dvr_entry_get_start_time(de);
  stop  = dvr_entry_get_stop_time(de);
  if (stop > start)
    tm = stop - start;
  else
    tm = 0;
  return &tm;
}

static const void *
dvr_entry_class_status_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  static char buf[100];
  strncpy(buf, dvr_entry_status(de), sizeof(buf));
  buf[sizeof(buf)-1] = '\0';
  s = buf;
  return &s;
}

static const void *
dvr_entry_class_sched_status_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  static const char *s;
  static char buf[100];
  strncpy(buf, dvr_entry_schedstatus(de), sizeof(buf));
  buf[sizeof(buf)-1] = '\0';
  s = buf;
  return &s;
}

static const void *
dvr_entry_class_channel_icon_url_get(void *o)
{
  dvr_entry_t *de = (dvr_entry_t *)o;
  channel_t *ch = de->de_channel;
  static const char *s;
  if (ch == NULL) {
    s = "";
  } else if ((s = channel_get_icon(ch)) == NULL) {
    s = "";
  }
  return &s;
}

static const void *
dvr_entry_class_duplicate_get(void *o)
{
  static time_t null = 0;
  dvr_entry_t *de = (dvr_entry_t *)o;
  de = _dvr_duplicate_event(de);
  return de ? &de->de_start : &null;
}

htsmsg_t *
dvr_entry_class_duration_list(void *o, const char *not_set, int max, int step)
{
  int i;
  htsmsg_t *e, *l = htsmsg_create_list();
  char buf[32];
  e = htsmsg_create_map();
  htsmsg_add_u32(e, "key", 0);
  htsmsg_add_str(e, "val", not_set);
  htsmsg_add_msg(l, NULL, e);
  for (i = 1; i <= 120;  i++) {
    snprintf(buf, sizeof(buf), "%d min%s", i, i > 1 ? "s" : "");
    e = htsmsg_create_map();
    htsmsg_add_u32(e, "key", i * step);
    htsmsg_add_str(e, "val", buf);
    htsmsg_add_msg(l, NULL, e);
  }
  for (i = 120; i <= max; i += 30) {
    if ((i % 60) == 0)
      snprintf(buf, sizeof(buf), "%d hrs", i / 60);
    else
      snprintf(buf, sizeof(buf), "%d hrs %d min%s", i / 60, i % 60, (i % 60) > 0 ? "s" : "");
    e = htsmsg_create_map();
    htsmsg_add_u32(e, "key", i * step);
    htsmsg_add_str(e, "val", buf);
    htsmsg_add_msg(l, NULL, e);
  }
  return l;
}

static htsmsg_t *
dvr_entry_class_extra_list(void *o)
{
  return dvr_entry_class_duration_list(o, "Not set (use channel or DVR config)", 4*60, 1);
}
                                        
static htsmsg_t *
dvr_entry_class_content_type_list(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "epg/content_type/list");
  return m;
}

const idclass_t dvr_entry_class = {
  .ic_class     = "dvrentry",
  .ic_caption   = "DVR Entry",
  .ic_event     = "dvrentry",
  .ic_save      = dvr_entry_class_save,
  .ic_get_title = dvr_entry_class_get_title,
  .ic_delete    = dvr_entry_class_delete,
  .ic_perm      = dvr_entry_class_perm,
  .ic_properties = (const property_t[]) {
    {
      .type     = PT_TIME,
      .id       = "start",
      .name     = "Start Time",
      .set      = dvr_entry_class_start_set,
      .off      = offsetof(dvr_entry_t, de_start),
      .get_opts = dvr_entry_class_start_opts,
    },
    {
      .type     = PT_TIME,
      .id       = "start_extra",
      .name     = "Extra Start Time",
      .off      = offsetof(dvr_entry_t, de_start_extra),
      .set      = dvr_entry_class_start_extra_set,
      .list     = dvr_entry_class_extra_list,
      .get_opts = dvr_entry_class_start_extra_opts,
      .opts     = PO_DURATION | PO_SORTKEY,
    },
    {
      .type     = PT_TIME,
      .id       = "start_real",
      .name     = "Scheduled Start Time",
      .get      = dvr_entry_class_start_real_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_TIME,
      .id       = "stop",
      .name     = "Stop Time",
      .set      = dvr_entry_class_stop_set,
      .off      = offsetof(dvr_entry_t, de_stop),
    },
    {
      .type     = PT_TIME,
      .id       = "stop_extra",
      .name     = "Extra Stop Time",
      .off      = offsetof(dvr_entry_t, de_stop_extra),
      .list     = dvr_entry_class_extra_list,
      .opts     = PO_DURATION | PO_SORTKEY,
    },
    {
      .type     = PT_TIME,
      .id       = "stop_real",
      .name     = "Scheduled Stop Time",
      .get      = dvr_entry_class_stop_real_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_TIME,
      .id       = "duration",
      .name     = "Duration",
      .get      = dvr_entry_class_duration_get,
      .opts     = PO_RDONLY | PO_NOSAVE | PO_DURATION,
    },
    {
      .type     = PT_STR,
      .id       = "channel",
      .name     = "Channel",
      .set      = dvr_entry_class_channel_set,
      .get      = dvr_entry_class_channel_get,
      .rend     = dvr_entry_class_channel_rend,
      .list     = channel_class_get_list,
      .get_opts = dvr_entry_class_start_opts,
    },
    {
      .type     = PT_STR,
      .id       = "channel_icon",
      .name     = "Channel Icon",
      .get      = dvr_entry_class_channel_icon_url_get,
      .opts     = PO_HIDDEN | PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_STR,
      .id       = "channelname",
      .name     = "Channel Name",
      .get      = dvr_entry_class_channel_name_get,
      .set      = dvr_entry_class_channel_name_set,
      .off      = offsetof(dvr_entry_t, de_channel_name),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_LANGSTR,
      .id       = "title",
      .name     = "Title",
      .off      = offsetof(dvr_entry_t, de_title),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "disp_title",
      .name     = "Title",
      .get      = dvr_entry_class_disp_title_get,
      .set      = dvr_entry_class_disp_title_set,
      .opts     = PO_NOSAVE,
    },
    {
      .type     = PT_LANGSTR,
      .id       = "subtitle",
      .name     = "Subtitle",
      .off      = offsetof(dvr_entry_t, de_subtitle),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "disp_subtitle",
      .name     = "Subtitle",
      .get      = dvr_entry_class_disp_subtitle_get,
      .set      = dvr_entry_class_disp_subtitle_set,
      .opts     = PO_NOSAVE,
    },
    {
      .type     = PT_LANGSTR,
      .id       = "description",
      .name     = "Description",
      .off      = offsetof(dvr_entry_t, de_desc),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "disp_description",
      .name     = "Description",
      .get      = dvr_entry_class_disp_description_get,
      .opts     = PO_RDONLY | PO_NOSAVE | PO_HIDDEN,
    },
    {
      .type     = PT_INT,
      .id       = "pri",
      .name     = "Priority",
      .off      = offsetof(dvr_entry_t, de_pri),
      .def.i    = DVR_PRIO_NORMAL,
      .set      = dvr_entry_class_pri_set,
      .list     = dvr_entry_class_pri_list,
      .opts     = PO_SORTKEY,
    },
    {
      .type     = PT_INT,
      .id       = "retention",
      .name     = "Retention",
      .off      = offsetof(dvr_entry_t, de_retention),
      .set      = dvr_entry_class_retention_set,
    },
    {
      .type     = PT_INT,
      .id       = "container",
      .name     = "Container",
      .off      = offsetof(dvr_entry_t, de_mc),
      .def.i    = MC_MATROSKA,
      .set      = dvr_entry_class_mc_set,
      .list     = dvr_entry_class_mc_list,
      .opts     = PO_RDONLY
    },
    {
      .type     = PT_STR,
      .id       = "config_name",
      .name     = "DVR Configuration",
      .set      = dvr_entry_class_config_name_set,
      .get      = dvr_entry_class_config_name_get,
      .list     = dvr_entry_class_config_name_list,
      .rend     = dvr_entry_class_config_name_rend,
      .get_opts = dvr_entry_class_start_opts,
    },
    {
      .type     = PT_STR,
      .id       = "owner",
      .name     = "Owner",
      .off      = offsetof(dvr_entry_t, de_owner),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "creator",
      .name     = "Creator",
      .off      = offsetof(dvr_entry_t, de_creator),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "filename",
      .name     = "Filename",
      .off      = offsetof(dvr_entry_t, de_filename),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "directory",
      .name     = "Directory",
      .off      = offsetof(dvr_entry_t, de_directory),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_U32,
      .id       = "errorcode",
      .name     = "Error Code",
      .off      = offsetof(dvr_entry_t, de_last_error),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_U32,
      .id       = "errors",
      .name     = "Errors",
      .off      = offsetof(dvr_entry_t, de_errors),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_U32,
      .id       = "data_errors",
      .name     = "Data Errors",
      .off      = offsetof(dvr_entry_t, de_data_errors),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_U16,
      .id       = "dvb_eid",
      .name     = "DVB EPG ID",
      .off      = offsetof(dvr_entry_t, de_dvb_eid),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_BOOL,
      .id       = "noresched",
      .name     = "Do Not Reschedule",
      .off      = offsetof(dvr_entry_t, de_dont_reschedule),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "autorec",
      .name     = "Auto Record",
      .set      = dvr_entry_class_autorec_set,
      .get      = dvr_entry_class_autorec_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "timerec",
      .name     = "Auto Time Record",
      .set      = dvr_entry_class_timerec_set,
      .get      = dvr_entry_class_timerec_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_U32,
      .id       = "content_type",
      .name     = "Content Type",
      .list     = dvr_entry_class_content_type_list,
      .off      = offsetof(dvr_entry_t, de_content_type),
      .opts     = PO_RDONLY | PO_SORTKEY,
    },
    {
      .type     = PT_U32,
      .id       = "broadcast",
      .name     = "Broadcast",
      .set      = dvr_entry_class_broadcast_set,
      .get      = dvr_entry_class_broadcast_get,
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "episode",
      .name     = "Episode",
      .off      = offsetof(dvr_entry_t, de_episode),
      .opts     = PO_RDONLY | PO_HIDDEN,
    },
    {
      .type     = PT_STR,
      .id       = "url",
      .name     = "URL",
      .get      = dvr_entry_class_url_get,
      .opts     = PO_RDONLY | PO_NOSAVE | PO_HIDDEN,
    },
    {
      .type     = PT_S64,
      .id       = "filesize",
      .name     = "File Size",
      .get      = dvr_entry_class_filesize_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_STR,
      .id       = "status",
      .name     = "Status",
      .get      = dvr_entry_class_status_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_STR,
      .id       = "sched_status",
      .name     = "Schedule Status",
      .get      = dvr_entry_class_sched_status_get,
      .opts     = PO_RDONLY | PO_NOSAVE | PO_HIDDEN,
    },
    {
      .type     = PT_TIME,
      .id       = "duplicate",
      .name     = "Rerun of",
      .get      = dvr_entry_class_duplicate_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_STR,
      .id       = "comment",
      .name     = "Comment",
      .off      = offsetof(dvr_entry_t, de_comment),
    },
    {}
  }
};

/**
 *
 */
void
dvr_destroy_by_channel(channel_t *ch, int delconf)
{
  dvr_entry_t *de;

  while((de = LIST_FIRST(&ch->ch_dvrs)) != NULL) {
    LIST_REMOVE(de, de_channel_link);
    de->de_channel = NULL;
    free(de->de_channel_name);
    de->de_channel_name = strdup(channel_get_name(ch));
    dvr_entry_purge(de, delconf);
  }
}

/**
 *
 */
int64_t
dvr_get_filesize(dvr_entry_t *de)
{
  struct stat st;

  if(de->de_filename == NULL)
    return -1;

  if(stat(de->de_filename, &st) != 0)
    return -1;

  return st.st_size;
}



/**
 *
 */
static struct strtab priotab[] = {
  { "important",   DVR_PRIO_IMPORTANT },
  { "high",        DVR_PRIO_HIGH },
  { "normal",      DVR_PRIO_NORMAL },
  { "low",         DVR_PRIO_LOW },
  { "unimportant", DVR_PRIO_UNIMPORTANT }
};

dvr_prio_t
dvr_pri2val(const char *s)
{
  return str2val_def(s, priotab, DVR_PRIO_NORMAL);
}

const char *
dvr_val2pri(dvr_prio_t v)
{
  return val2str(v, priotab) ?: "invalid";
}


/**
 *
 */
void
dvr_entry_delete(dvr_entry_t *de)
{
  time_t t;
  struct tm tm;
  char tbuf[64];

  t = dvr_entry_get_start_time(de);
  localtime_r(&t, &tm);
  if (strftime(tbuf, sizeof(tbuf), "%F %T", &tm) <= 0)
    *tbuf = 0;

  tvhlog(LOG_INFO, "dvr", "delete entry %s \"%s\" on \"%s\" start time %s, "
	 "scheduled for recording by \"%s\", retention %d days",
         idnode_uuid_as_str(&de->de_id),
	 lang_str_get(de->de_title, NULL), DVR_CH_NAME(de), tbuf,
	 de->de_creator ?: "",
	 dvr_entry_get_retention(de));

  if(de->de_filename != NULL) {
#if ENABLE_INOTIFY
    dvr_inotify_del(de);
#endif
    if(unlink(de->de_filename) && errno != ENOENT)
      tvhlog(LOG_WARNING, "dvr", "Unable to remove file '%s' from disk -- %s",
	     de->de_filename, strerror(errno));

    /* Also delete directories, if they were created for the recording and if they are empty */

    dvr_config_t *cfg = de->de_config;
    char path[500];

    snprintf(path, sizeof(path), "%s", cfg->dvr_storage);

    if(cfg->dvr_title_dir || cfg->dvr_channel_dir || cfg->dvr_dir_per_day || de->de_directory) {
      char *p;
      int l;

      l = strlen(de->de_filename);
      p = alloca(l + 1);
      memcpy(p, de->de_filename, l);
      p[l--] = 0;

      for(; l >= 0; l--) {
        if(p[l] == '/') {
          p[l] = 0;
          if(strncmp(p, cfg->dvr_storage, strlen(p)) == 0)
            break;
          if(rmdir(p) == -1)
            break;
        }
      }
    }

  }
  dvr_entry_destroy(de, 1);
}

/**
 *
 */
dvr_entry_t *
dvr_entry_cancel(dvr_entry_t *de)
{
  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    dvr_entry_destroy(de, 1);
    return NULL;

  case DVR_RECORDING:
    de->de_dont_reschedule = 1;
    dvr_stop_recording(de, SM_CODE_ABORTED, 1);
    return de;

  case DVR_COMPLETED:
  case DVR_MISSED_TIME:
  case DVR_NOSTATE:
    dvr_entry_destroy(de, 1);
    return NULL;

  default:
    abort();
  }
}

/**
 *
 */
void
dvr_entry_cancel_delete(dvr_entry_t *de)
{
  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    dvr_entry_destroy(de, 1);
    break;

  case DVR_RECORDING:
    de->de_dont_reschedule = 1;
    dvr_stop_recording(de, SM_CODE_ABORTED, 1);
  case DVR_COMPLETED:
    dvr_entry_delete(de);
    break;

  case DVR_MISSED_TIME:
  case DVR_NOSTATE:
    dvr_entry_destroy(de, 1);
    break;

  default:
    abort();
  }
}

/**
 *
 */
void
dvr_entry_init(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  if((l = hts_settings_load("dvr/log")) != NULL) {
    HTSMSG_FOREACH(f, l) {
      if((c = htsmsg_get_map_by_field(f)) == NULL)
	continue;
      (void)dvr_entry_create(f->hmf_name, c);
    }
    htsmsg_destroy(l);
  }
}

void
dvr_entry_done(void)
{
  dvr_entry_t *de;
  lock_assert(&global_lock);
  while ((de = LIST_FIRST(&dvrentries)) != NULL)
      dvr_entry_destroy(de, 0);
}
