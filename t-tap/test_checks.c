/*****************************************************************************
* 
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* 
* 
*****************************************************************************/

#define NSCORE 1
#include "config.h"
#include "comments.h"
#include "common.h"
#include "statusdata.h"
#include "downtime.h"
#include "macros.h"
#include "nagios.h"
#include "broker.h"
#include "perfdata.h"
#include "tap.h"
#include "test-stubs.c"

/* Test specific functions + variables */
service *svc1=NULL, *svc2=NULL;
host *host1=NULL;
int found_log_rechecking_host_when_service_wobbles=0;
int found_log_run_async_host_check_3x=0;
check_result *tmp_check_result;

void setup_check_result() {
	struct timeval start_time,finish_time;
	start_time.tv_sec=1234567890L;
	start_time.tv_usec=0L;
	finish_time.tv_sec=1234567891L;
	finish_time.tv_usec=0L;

	tmp_check_result=(check_result *)malloc(sizeof(check_result));
	tmp_check_result->check_type=SERVICE_CHECK_ACTIVE;
	tmp_check_result->check_options=0;
	tmp_check_result->scheduled_check=TRUE;
	tmp_check_result->reschedule_check=TRUE;
	tmp_check_result->exited_ok=TRUE;
	tmp_check_result->return_code=0;
	tmp_check_result->output=strdup("Fake result");
	tmp_check_result->latency=0.6969;
	tmp_check_result->start_time=start_time;
	tmp_check_result->finish_time=finish_time;
}

int c=0;
int update_program_status(int aggregated_dump){ 
	c++;
	/* printf ("# In the update_program_status hook: %d\n", c); */
	
	/* Set this to break out of event_execution_loop */
	if (c>10) {
		sigshutdown=TRUE;
		c=0;
	}
} 
int log_debug_info(int level, int verbosity, const char *fmt, ...){
	va_list ap;
	char *buffer=NULL;

	va_start(ap, fmt);
	/* vprintf( fmt, ap ); */
	vasprintf(&buffer,fmt,ap);
	if(strcmp(buffer,"Service wobbled between non-OK states, so we'll recheck the host state...\n")==0) {
	    found_log_rechecking_host_when_service_wobbles++;
	}
	if(strcmp(buffer,"run_async_host_check_3x()\n")==0) {
	    found_log_run_async_host_check_3x++;
	}
	free(buffer);
	va_end(ap);
}

void
setup_objects(time_t time) {
	timed_event *new_event=NULL;

	host1=(host *)calloc(1, sizeof(host));
	host1->name=strdup("Host1");
	host1->address=strdup("127.0.0.1");
	host1->retry_interval=1;
	host1->check_interval=5;
	host1->check_options=0;
	host1->state_type=SOFT_STATE;
	host1->current_state=HOST_DOWN;
	host1->has_been_checked=TRUE;
	host1->last_check=time;
	host1->next_check=time;

	/* First service is a normal one */
	svc1=(service *)calloc(1, sizeof(service));
	svc1->host_name=strdup("Host1");
	svc1->host_ptr=host1;
	svc1->description=strdup("Normal service");
	svc1->check_options=0;
	svc1->next_check=time;
	svc1->state_type=SOFT_STATE;
	svc1->current_state=STATE_CRITICAL;
	svc1->retry_interval=1;
	svc1->check_interval=5;
	svc1->current_attempt=1;
	svc1->max_attempts=4;
	svc1->last_state_change=0;
	svc1->last_state_change=0;
	svc1->last_check=(time_t)1234560000;
	svc1->host_problem_at_last_check=FALSE;
	svc1->plugin_output=strdup("Initial state");
	svc1->last_hard_state_change=(time_t)1111111111;

	/* Second service .... to be configured! */
	svc2=(service *)calloc(1, sizeof(service));
	svc2->host_name=strdup("Host1");
	svc2->description=strdup("To be nudged");
	svc2->check_options=0;
	svc2->next_check=time;
	svc2->state_type=SOFT_STATE;
	svc2->current_state=STATE_OK;
	svc2->retry_interval=1;
	svc2->check_interval=5;

}

int
main (int argc, char **argv)
{
	time_t now=0L;


	plan_tests(11);

	time(&now);

	
	/* Test to confirm that if a service is warning, the notified_on_critical is reset */
	tmp_check_result=(check_result *)calloc(1, sizeof(check_result));
	tmp_check_result->host_name=strdup("host1");
	tmp_check_result->service_description=strdup("Normal service");
	tmp_check_result->object_check_type=SERVICE_CHECK;
	tmp_check_result->check_type=SERVICE_CHECK_ACTIVE;
	tmp_check_result->check_options=0;
	tmp_check_result->scheduled_check=TRUE;
	tmp_check_result->reschedule_check=TRUE;
	tmp_check_result->latency=0.666;
	tmp_check_result->start_time.tv_sec=1234567890;
	tmp_check_result->start_time.tv_usec=56565;
	tmp_check_result->finish_time.tv_sec=1234567899;
	tmp_check_result->finish_time.tv_usec=45454;
	tmp_check_result->early_timeout=0;
	tmp_check_result->exited_ok=TRUE;
	tmp_check_result->return_code=1;
	tmp_check_result->output=strdup("Warning - check notified_on_critical reset");

	setup_objects(now);
	svc1->last_state=STATE_CRITICAL;
	svc1->notified_on_critical=TRUE;
	svc1->current_notification_number=999;
	svc1->last_notification=(time_t)11111;
	svc1->next_notification=(time_t)22222;
	svc1->no_more_notifications=TRUE;

	handle_async_service_check_result( svc1, tmp_check_result );

	/* This has been taken out because it is not required
	ok( svc1->notified_on_critical==FALSE, "notified_on_critical reset" );
	*/
	ok( svc1->last_notification==(time_t)0, "last notification reset due to state change" );
	ok( svc1->next_notification==(time_t)0, "next notification reset due to state change" );
	ok( svc1->no_more_notifications==FALSE, "no_more_notifications reset due to state change" );
	ok( svc1->current_notification_number==999, "notification number NOT reset" );

	/* Test case:
		service that transitions from OK to CRITICAL (where its host is set to DOWN) will get set to a hard state 
		even though check attempts = 1 of 4
	*/
	setup_objects((time_t) 1234567800L);
	host1->current_state=HOST_DOWN;
	svc1->current_state=STATE_OK;
	svc1->state_type=HARD_STATE;
	setup_check_result();
	tmp_check_result->return_code=STATE_CRITICAL;
	tmp_check_result->output=strdup("CRITICAL failure");
	log_service_event_flag=0;

	handle_async_service_check_result(svc1, tmp_check_result);

	ok( log_service_event_flag==1, "log_service_event() was called");
	ok( svc1->last_hard_state_change == (time_t)1234567890, "Got last_hard_state_change time=%lu", svc1->last_hard_state_change);
	ok( svc1->last_state_change == svc1->last_hard_state_change, "Got same last_state_change" );
	ok( svc1->last_hard_state == 2, "Should save the last hard state as critical for next time");
	ok( svc1->host_problem_at_last_check==TRUE, "Got host_problem_at_last_check set to TRUE due to host failure - this needs to be saved otherwise extra alerts raised in subsequent runs");
	ok( svc1->state_type == HARD_STATE, "This should be a HARD state since the host is in a failure state");
	ok( svc1->current_attempt==1, "Previous status was OK, so this failure should show current_attempt=1") || diag("Current attempt=%d", svc1->current_attempt);

	return exit_status ();
}

