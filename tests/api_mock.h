/*
 * A Jellyfin server in the process, for the UI tests: api_mock.c answers the
 * fetcher's tasks from a fixed library instead of api_http.c going to the
 * network, and a test can make any job fail or hang.
 *
 * The library: two libraries (Movies with MOCK_MOVIES films, Shows with
 * MOCK_SERIES series of MOCK_SEASONS seasons of MOCK_EPISODES episodes), a
 * resume row of MOCK_RESUME films and a next-up row of MOCK_NEXT_UP episodes.
 * Ids say what they are: movie-7, series-2, season-2-1, episode-2-1-5; mock_id
 * builds them.
 */
#pragma once

#include <stdbool.h>

#include "jf/api.h"

#define MOCK_MOVIES 120
#define MOCK_SERIES 30
#define MOCK_SEASONS 3
#define MOCK_EPISODES 12
#define MOCK_RESUME 12
/* Each library's recently added: films 100 on, or the first shows. */
#define MOCK_LATEST 4
#define MOCK_NEXT_UP 10

/* Back to the library as above: no failures, nothing held, no calls counted.
 * Releases whatever is held first. */
void mock_reset(void);
/* The next `count` requests of `job` fail as an answer of HTTP `status` would:
 * 401 or 403 is Unauthorized, another 4xx ClientError, 5xx ServerError, 0 a
 * timeout or a refused connection (RequestFailed). A `count` of 0 fails every
 * one until reset. */
void mock_fail(jf_job job, int status, unsigned count);
/* Requests of `job` wait, holding their task in flight, until released: a slow
 * server, so a test can navigate while one is outstanding. Whatever mock_fail
 * says for the job at release is what they answer. */
void mock_hold(jf_job job);
void mock_release(jf_job job);
/* Requests waiting in mock_hold right now. */
unsigned mock_held(void);
/* The JSON the last JF_JOB_SAVE_DELAYS sent; empty since reset. */
const char *mock_saved_delays(void);
/* The server's event socket saying the user's items changed: these, or with
 * none, something. */
void mock_event(const jf_user_data *items, size_t count);
/* The position the last progress report gave, in milliseconds. */
uint64_t mock_progress_ms(void);
/* Requests of `job` answered or failed since reset. */
unsigned mock_calls(jf_job job);

/* "movie-%u", "series-%u", ... into a static buffer, for comparing with card
 * ids. */
const char *mock_id(const char *pattern, ...);
