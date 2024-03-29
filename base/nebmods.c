/*****************************************************************************
 *
 * NEBMODS.C - Event Broker Module Functions
 *
 * Copyright (c) 2002-2008 Ethan Galstad (egalstad@nagios.org)
 * Last Modified: 11-02-2008
 *
 * License:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *****************************************************************************/

#include "../include/config.h"
#include "../include/common.h"
#include "../include/nebmods.h"
#include "../include/neberrors.h"
#include "../include/nagios.h"


#ifdef USE_EVENT_BROKER


nebmodule *neb_module_list=NULL;
nebcallback **neb_callback_list=NULL;

extern char     *temp_path;


/*#define DEBUG*/


/****************************************************************************/
/****************************************************************************/
/* INITIALIZATION/CLEANUP FUNCTIONS                                         */
/****************************************************************************/
/****************************************************************************/

/* initialize module routines */
int neb_init_modules(void){
#ifdef USE_LTDL
	int result=OK;
#endif

	/* initialize library */
#ifdef USE_LTDL
	result=lt_dlinit();
	if(result)
		return ERROR;
#endif

	return OK;
        }


/* deinitialize module routines */
int neb_deinit_modules(void){
#ifdef USE_LTDL
	int result=OK;
#endif

	/* deinitialize library */
#ifdef USE_LTDL
	result=lt_dlexit();
	if(result)
		return ERROR;
#endif

	return OK;
        }



/* add a new module to module list */
int neb_add_module(char *filename,char *args,int should_be_loaded){
	nebmodule *new_module=NULL;
	int x=OK;

	if(filename==NULL)
		return ERROR;

	/* allocate memory */
	new_module=(nebmodule *)malloc(sizeof(nebmodule));
	if(new_module==NULL)
		return ERROR;

	/* initialize vars */
	new_module->filename=(char *)strdup(filename);
	new_module->args=(args==NULL)?NULL:(char *)strdup(args);
	new_module->should_be_loaded=should_be_loaded;
	new_module->is_currently_loaded=FALSE;
	for(x=0;x<NEBMODULE_MODINFO_NUMITEMS;x++)
		new_module->info[x]=NULL;
	new_module->module_handle=NULL;
	new_module->init_func=NULL;
	new_module->deinit_func=NULL;
#ifdef HAVE_PTHREAD_H
	new_module->thread_id=(pthread_t)NULL;
#endif

	/* add module to head of list */
	new_module->next=neb_module_list;
	neb_module_list=new_module;

	log_debug_info(DEBUGL_EVENTBROKER,0,"Added module: name='%s', args='%s', should_be_loaded='%d'\n",filename,args,should_be_loaded);

	return OK;
        }


/* free memory allocated to module list */
int neb_free_module_list(void){
	nebmodule *temp_module=NULL;
	nebmodule *next_module=NULL;
	int x=OK;

	for(temp_module=neb_module_list;temp_module;){
		next_module=temp_module->next;
		my_free(temp_module->filename);
		my_free(temp_module->args);
		for(x=0;x<NEBMODULE_MODINFO_NUMITEMS;x++)
			my_free(temp_module->info[x]);
		my_free(temp_module);
		temp_module=next_module;
	        }

	neb_module_list=NULL;

	return OK;
        }



/****************************************************************************/
/****************************************************************************/
/* LOAD/UNLOAD FUNCTIONS                                                    */
/****************************************************************************/
/****************************************************************************/


/* load all modules */
int neb_load_all_modules(void){
	nebmodule *temp_module=NULL;
	int result=OK;

	for(temp_module=neb_module_list;temp_module;temp_module=temp_module->next){
		result=neb_load_module(temp_module);
	        }

	return OK;
        }


#ifndef PATH_MAX
# define PATH_MAX 4096
#endif
/* load a particular module */
int neb_load_module(nebmodule *mod)
{
	int (*initfunc)(int,char *,void *);
	int *module_version_ptr=NULL;
	char output_file[PATH_MAX];
	int dest_fd, result=OK;

	if(mod==NULL || mod->filename==NULL)
		return ERROR;
	
	/* don't reopen the module */
	if(mod->is_currently_loaded==TRUE)
		return OK;

	/* don't load modules unless they should be loaded */
	if(mod->should_be_loaded==FALSE)
		return ERROR;

	/********** 
	   Using dlopen() is great, but a real danger as-is.  The problem with loaded modules is that if you overwrite the original file (e.g. using 'mv'),
	   you do not alter the inode of the original file.  Since the original file/module is memory-mapped in some fashion, Nagios will segfault the next
	   time an event broker call is directed to one of the module's callback functions.  This is extremely problematic when it comes to upgrading NEB
	   modules while Nagios is running.  A workaround is to (1) 'mv' the original/loaded module file to another name (on the same filesystem)
	   and (2) copy the new module file to the location of the original one (using the original filename).  In this scenario, dlopen() will keep referencing
	   the original file/inode for callbacks.  This is not an ideal solution.   A better one is to delete the module file once it is loaded by dlopen().
	   This prevents other processed from unintentially overwriting the original file, which would cause Nagios to crash.  However, if we delete the file
	   before anyone else can muck with it, things should be good.  'lsof' shows that a deleted file is still referenced by the kernel and callback
	   functions continue to work once the module has been loaded.  Long story, but this took quite a while to figure out, as there isn't much 
	   of anything I could find on the subject other than some sketchy info on similar problems on HP-UX.  Hopefully this will save future coders some time.
	   So... the trick is to (1) copy the module to a temp file, (2) dlopen() the temp file, and (3) immediately delete the temp file. 
	************/

	/*
	 * open a temp file for copying the module. We use my_fdcopy() so
	 * we re-use the destination file descriptor returned by mkstemp(3),
	 * which we have to close ourselves.
	 */
	snprintf(output_file, sizeof(output_file) - 1, "%s/nebmodXXXXXX",temp_path);
	dest_fd = mkstemp(output_file);
	result = my_fdcopy(mod->filename, output_file, dest_fd);
	close(dest_fd);
	if (result == ERROR) {
		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Failed to safely copy module '%s'. The module will not be loaded\n", mod->filename);
		return ERROR;
	}

	/* load the module (use the temp copy we just made) */
#ifdef USE_LTDL
	mod->module_handle=lt_dlopen(output_file);
#else
	mod->module_handle=(void *)dlopen(output_file,RTLD_NOW|RTLD_GLOBAL);
#endif
	if(mod->module_handle==NULL){

#ifdef USE_LTDL
		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Could not load module '%s' -> %s\n",mod->filename,lt_dlerror());
#else
		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Could not load module '%s' -> %s\n",mod->filename,dlerror());
#endif

		return ERROR;
	        }

	/* mark the module as being loaded */
	mod->is_currently_loaded=TRUE;

	/* delete the temp copy of the module we just created and loaded */
	/* this will prevent other processes from overwriting the file (using the same inode), which would cause Nagios to crash */
	/* the kernel will keep the deleted file in memory until we unload it */
	/* NOTE: This *should* be portable to most Unices, but I've only tested it on Linux */
	if(unlink(output_file)==-1){
		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Could not delete temporary file '%s' used for module '%s'.  The module will be unloaded: %s\n",output_file,mod->filename,strerror(errno));
		neb_unload_module(mod,NEBMODULE_FORCE_UNLOAD,NEBMODULE_ERROR_API_VERSION);

		return ERROR;
		}

	/* find module API version */
#ifdef USE_LTDL
	module_version_ptr=(int *)lt_dlsym(mod->module_handle,"__neb_api_version");
#else
	module_version_ptr=(int *)dlsym(mod->module_handle,"__neb_api_version");
#endif
	
	/* check the module API version */
	if(module_version_ptr==NULL || ((*module_version_ptr)!=CURRENT_NEB_API_VERSION)){

		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Module '%s' is using an old or unspecified version of the event broker API.  Module will be unloaded.\n",mod->filename);

		neb_unload_module(mod,NEBMODULE_FORCE_UNLOAD,NEBMODULE_ERROR_API_VERSION);

		return ERROR;
	        }

	/* locate the initialization function */
#ifdef USE_LTDL
	mod->init_func=lt_dlsym(mod->module_handle,"nebmodule_init");
#else
	mod->init_func=(void *)dlsym(mod->module_handle,"nebmodule_init");
#endif

	/* if the init function could not be located, unload the module */
	if(mod->init_func==NULL){

		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Could not locate nebmodule_init() in module '%s'.  Module will be unloaded.\n",mod->filename);

		neb_unload_module(mod,NEBMODULE_FORCE_UNLOAD,NEBMODULE_ERROR_NO_INIT);

		return ERROR;
	        }

	/* run the module's init function */
	initfunc=mod->init_func;
	result=(*initfunc)(NEBMODULE_NORMAL_LOAD,mod->args,mod->module_handle);

	/* if the init function returned an error, unload the module */
	if(result!=OK){

		logit(NSLOG_RUNTIME_ERROR,FALSE,"Error: Function nebmodule_init() in module '%s' returned an error.  Module will be unloaded.\n",mod->filename);

		neb_unload_module(mod,NEBMODULE_FORCE_UNLOAD,NEBMODULE_ERROR_BAD_INIT);

		return ERROR;
	        }

	logit(NSLOG_INFO_MESSAGE,FALSE,"Event broker module '%s' initialized successfully.\n",mod->filename);

	/* locate the de-initialization function (may or may not be present) */
#ifdef USE_LTDL
	mod->deinit_func=lt_dlsym(mod->module_handle,"nebmodule_deinit");
#else
	mod->deinit_func=(void *)dlsym(mod->module_handle,"nebmodule_deinit");
#endif

	log_debug_info(DEBUGL_EVENTBROKER,0,"Module '%s' loaded with return code of '%d'\n",mod->filename,result);
	if(mod->deinit_func!=NULL)
		log_debug_info(DEBUGL_EVENTBROKER,0,"nebmodule_deinit() found\n");

	return OK;
        }


/* close (unload) all modules that are currently loaded */
int neb_unload_all_modules(int flags, int reason){
	nebmodule *temp_module;

	for(temp_module=neb_module_list;temp_module;temp_module=temp_module->next){

		/* skip modules that are not loaded */
		if(temp_module->is_currently_loaded==FALSE)
			continue;

		/* skip modules that do not have a valid handle */
		if(temp_module->module_handle==NULL)
			continue;

		/* close/unload the module */
		neb_unload_module(temp_module,flags,reason);
	        }

	return OK;
        }



/* close (unload) a particular module */
int neb_unload_module(nebmodule *mod, int flags, int reason){
	int (*deinitfunc)(int,int);
	int result=OK;

	if(mod==NULL)
		return ERROR;

	log_debug_info(DEBUGL_EVENTBROKER,0,"Attempting to unload module '%s': flags=%d, reason=%d\n",mod->filename,flags,reason);

	/* call the de-initialization function if available (and the module was initialized) */
	if(mod->deinit_func && reason!=NEBMODULE_ERROR_BAD_INIT){

		deinitfunc=mod->deinit_func;

		/* module can opt to not be unloaded */
		result=(*deinitfunc)(flags,reason);

		/* if module doesn't want to be unloaded, exit with error (unless its being forced) */
		if(result!=OK && !(flags & NEBMODULE_FORCE_UNLOAD))
			return ERROR;
	        }

	/* deregister all of the module's callbacks */
	neb_deregister_module_callbacks(mod);

	/* unload the module */
#ifdef USE_LTDL
	result=lt_dlclose(mod->module_handle);
#else
	result=dlclose(mod->module_handle);
#endif

	/* mark the module as being unloaded */
	mod->is_currently_loaded=FALSE;

	log_debug_info(DEBUGL_EVENTBROKER,0,"Module '%s' unloaded successfully.\n",mod->filename);

	logit(NSLOG_INFO_MESSAGE,FALSE,"Event broker module '%s' deinitialized successfully.\n",mod->filename);

	return OK;
        }




/****************************************************************************/
/****************************************************************************/
/* INFO FUNCTIONS                                                           */
/****************************************************************************/
/****************************************************************************/

/* sets module information */
int neb_set_module_info(void *handle, int type, char *data){
	nebmodule *temp_module=NULL;

	if(handle==NULL)
		return NEBERROR_NOMODULE;

	/* check type */
	if(type<0 || type>=NEBMODULE_MODINFO_NUMITEMS)
		return NEBERROR_MODINFOBOUNDS;

	/* find the module */
	for(temp_module=neb_module_list;temp_module!=NULL;temp_module=temp_module->next){
		if((void *)temp_module->module_handle==(void *)handle)
			break;
		}
	if(temp_module==NULL)
		return NEBERROR_BADMODULEHANDLE;

	/* free any previously allocated memory */
	my_free(temp_module->info[type]);

	/* allocate memory for the new data */
	if((temp_module->info[type]=(char *)strdup(data))==NULL)
		return NEBERROR_NOMEM;

	return OK;
        }



/****************************************************************************/
/****************************************************************************/
/* CALLBACK FUNCTIONS                                                       */
/****************************************************************************/
/****************************************************************************/

/* allows a module to register a callback function */
int neb_register_callback(int callback_type, void *mod_handle, int priority, int (*callback_func)(int,void *)){
	nebmodule *temp_module=NULL;
	nebcallback *new_callback=NULL;
	nebcallback *temp_callback=NULL;
	nebcallback *last_callback=NULL;

	if(callback_func==NULL)
		return NEBERROR_NOCALLBACKFUNC;

	if(neb_callback_list==NULL)
		return NEBERROR_NOCALLBACKLIST;

	if(mod_handle==NULL)
		return NEBERROR_NOMODULEHANDLE;

	/* make sure the callback type is within bounds */
	if(callback_type<0 || callback_type>=NEBCALLBACK_NUMITEMS)
		return NEBERROR_CALLBACKBOUNDS;

	/* make sure module handle is valid */
	for(temp_module=neb_module_list;temp_module;temp_module=temp_module->next){
		if((void *)temp_module->module_handle==(void *)mod_handle)
			break;
	        }
	if(temp_module==NULL)
		return NEBERROR_BADMODULEHANDLE;

	/* allocate memory */
	new_callback=(nebcallback *)malloc(sizeof(nebcallback));
	if(new_callback==NULL)
		return NEBERROR_NOMEM;
	
	new_callback->priority=priority;
	new_callback->module_handle=(void *)mod_handle;
	new_callback->callback_func=(void *)callback_func;

	/* add new function to callback list, sorted by priority (first come, first served for same priority) */
	new_callback->next=NULL;
	if(neb_callback_list[callback_type]==NULL)
		neb_callback_list[callback_type]=new_callback;
	else{
		last_callback=NULL;
		for(temp_callback=neb_callback_list[callback_type];temp_callback!=NULL;temp_callback=temp_callback->next){
			if(temp_callback->priority>new_callback->priority)
				break;
			last_callback=temp_callback;
	                }
		if(last_callback==NULL)
			neb_callback_list[callback_type]=new_callback;
		else{
			if(temp_callback==NULL)
				last_callback->next=new_callback;
			else{
				new_callback->next=temp_callback;
				last_callback->next=new_callback;
			        }
		        }
	        }

	return OK;
        }



/* dregisters all callback functions for a given module */
int neb_deregister_module_callbacks(nebmodule *mod){
	nebcallback *temp_callback=NULL;
	nebcallback *next_callback=NULL;
	int callback_type=0;

	if(mod==NULL)
		return NEBERROR_NOMODULE;

	if(neb_callback_list==NULL)
		return OK;

	for(callback_type=0;callback_type<NEBCALLBACK_NUMITEMS;callback_type++){
		for(temp_callback=neb_callback_list[callback_type];temp_callback!=NULL;temp_callback=next_callback){
			next_callback=temp_callback->next;
			if((void *)temp_callback->module_handle==(void *)mod->module_handle)
				neb_deregister_callback(callback_type,(int(*)(int,void*))temp_callback->callback_func);
		        }

	        }

	return OK;
        }


/* allows a module to deregister a callback function */
int neb_deregister_callback(int callback_type, int (*callback_func)(int,void *)){
	nebcallback *temp_callback=NULL;
	nebcallback *last_callback=NULL;
	nebcallback *next_callback=NULL;

	if(callback_func==NULL)
		return NEBERROR_NOCALLBACKFUNC;

	if(neb_callback_list==NULL)
		return NEBERROR_NOCALLBACKLIST;

	/* make sure the callback type is within bounds */
	if(callback_type<0 || callback_type>=NEBCALLBACK_NUMITEMS)
		return NEBERROR_CALLBACKBOUNDS;

	/* find the callback to remove */
	for(temp_callback=last_callback=neb_callback_list[callback_type];temp_callback!=NULL;temp_callback=next_callback){
		next_callback=temp_callback->next;

		/* we found it */
		if(temp_callback->callback_func==(void *)callback_func)
			break;

		last_callback=temp_callback;
	        }

	/* we couldn't find the callback */
	if(temp_callback==NULL)
		return NEBERROR_CALLBACKNOTFOUND;

	else{
		/* only one item in the list */
		if (temp_callback!=last_callback->next)
			neb_callback_list[callback_type]=NULL;
		else
			last_callback->next=next_callback;
		my_free(temp_callback);
		}
	
	return OK;
        }



/* make callbacks to modules */
int neb_make_callbacks(int callback_type, void *data){
	nebcallback *temp_callback, *next_callback;
	int (*callbackfunc)(int,void *);
	register int cbresult=0;
	int total_callbacks=0;

	/* make sure callback list is initialized */
	if(neb_callback_list==NULL)
		return ERROR;

	/* make sure the callback type is within bounds */
	if(callback_type<0 || callback_type>=NEBCALLBACK_NUMITEMS)
		return ERROR;

	log_debug_info(DEBUGL_EVENTBROKER,1,"Making callbacks (type %d)...\n",callback_type);

	/* make the callbacks... */
	for(temp_callback = neb_callback_list[callback_type];temp_callback;temp_callback=next_callback) {
		next_callback = temp_callback->next;
		callbackfunc=temp_callback->callback_func;
		cbresult=callbackfunc(callback_type,data);
		temp_callback = next_callback;

		total_callbacks++;
		log_debug_info(DEBUGL_EVENTBROKER,2,"Callback #%d (type %d) return code = %d\n",total_callbacks,callback_type,cbresult);

		/* module wants to cancel callbacks to other modules (and potentially cancel the default Nagios handling of an event) */
		if(cbresult==NEBERROR_CALLBACKCANCEL)
			break;

		/* module wants to override default Nagios handling of an event */
		/* not sure if we should bail out here just because one module wants to override things - what about other modules? EG 12/11/2006 */
		else if(cbresult==NEBERROR_CALLBACKOVERRIDE)
			break;
	        }

	return cbresult;
        }



/* initialize callback list */
int neb_init_callback_list(void){
	register int x=0;

	/* allocate memory for the callback list */
	neb_callback_list=(nebcallback **)malloc(NEBCALLBACK_NUMITEMS*sizeof(nebcallback *));
	if(neb_callback_list==NULL)
		return ERROR;

	/* initialize list pointers */
	for(x=0;x<NEBCALLBACK_NUMITEMS;x++)
		neb_callback_list[x]=NULL;

	return OK;
        }


/* free memory allocated to callback list */
int neb_free_callback_list(void){
	nebcallback *temp_callback=NULL;
	nebcallback *next_callback=NULL;
	register int x=0;

	if(neb_callback_list==NULL)
		return OK;

	for(x=0;x<NEBCALLBACK_NUMITEMS;x++){

		for(temp_callback=neb_callback_list[x];temp_callback!=NULL;temp_callback=next_callback){
			next_callback=temp_callback->next;
			my_free(temp_callback);
	                }

		neb_callback_list[x]=NULL;
	        }

	my_free(neb_callback_list);

	return OK;
        }

#endif
