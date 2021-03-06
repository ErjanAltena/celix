/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */
/*
 * framework.c
 *
 *  \date       Mar 23, 2010
 *  \author    	<a href="mailto:dev@celix.apache.org">Apache Celix Project Team</a>
 *  \copyright	Apache License, Version 2.0
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "celixbool.h"

#ifdef _WIN32
#include <winbase.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <uuid/uuid.h>

#include "framework_private.h"
#include "constants.h"
#include "resolver.h"
#include "utils.h"
#include "linked_list_iterator.h"
#include "service_reference_private.h"
#include "listener_hook_service.h"
#include "service_registration_private.h"

typedef celix_status_t (*create_function_pt)(bundle_context_pt context, void **userData);
typedef celix_status_t (*start_function_pt)(void * handle, bundle_context_pt context);
typedef celix_status_t (*stop_function_pt)(void * handle, bundle_context_pt context);
typedef celix_status_t (*destroy_function_pt)(void * handle, bundle_context_pt context);

struct activator {
    void * userData;
    start_function_pt start;
    stop_function_pt stop;
    destroy_function_pt destroy;
};

celix_status_t framework_setBundleStateAndNotify(framework_pt framework, bundle_pt bundle, int state);
celix_status_t framework_markBundleResolved(framework_pt framework, module_pt module);

celix_status_t framework_acquireBundleLock(framework_pt framework, bundle_pt bundle, int desiredStates);
bool framework_releaseBundleLock(framework_pt framework, bundle_pt bundle);

bool framework_acquireGlobalLock(framework_pt framework);
celix_status_t framework_releaseGlobalLock(framework_pt framework);

celix_status_t framework_acquireInstallLock(framework_pt framework, char * location);
celix_status_t framework_releaseInstallLock(framework_pt framework, char * location);

long framework_getNextBundleId(framework_pt framework);

celix_status_t fw_installBundle2(framework_pt framework, bundle_pt * bundle, long id, char * location, char *inputFile, bundle_archive_pt archive);

celix_status_t fw_refreshBundles(framework_pt framework, bundle_pt bundles[], int size);
celix_status_t fw_refreshBundle(framework_pt framework, bundle_pt bundle);

celix_status_t fw_populateDependentGraph(framework_pt framework, bundle_pt exporter, hash_map_pt *map);

celix_status_t fw_fireBundleEvent(framework_pt framework, bundle_event_type_e, bundle_pt bundle);
celix_status_t fw_fireFrameworkEvent(framework_pt framework, framework_event_type_e eventType, bundle_pt bundle, celix_status_t errorCode);
static void *fw_eventDispatcher(void *fw);

celix_status_t fw_invokeBundleListener(framework_pt framework, bundle_listener_pt listener, bundle_event_pt event, bundle_pt bundle);
celix_status_t fw_invokeFrameworkListener(framework_pt framework, framework_listener_pt listener, framework_event_pt event, bundle_pt bundle);

static celix_status_t framework_loadBundleLibraries(framework_pt framework, bundle_pt bundle);
static celix_status_t framework_loadLibraries(framework_pt framework, char *libraries, char *activator, bundle_archive_pt archive, void **activatorHandle);
static celix_status_t framework_loadLibrary(framework_pt framework, char *library, bundle_archive_pt archive, void **handle);

static celix_status_t frameworkActivator_start(void * userData, bundle_context_pt context);
static celix_status_t frameworkActivator_stop(void * userData, bundle_context_pt context);
static celix_status_t frameworkActivator_destroy(void * userData, bundle_context_pt context);


struct fw_refreshHelper {
    framework_pt framework;
    bundle_pt bundle;
    bundle_state_e oldState;
};

celix_status_t fw_refreshHelper_refreshOrRemove(struct fw_refreshHelper * refreshHelper);
celix_status_t fw_refreshHelper_restart(struct fw_refreshHelper * refreshHelper);
celix_status_t fw_refreshHelper_stop(struct fw_refreshHelper * refreshHelper);

struct fw_serviceListener {
	bundle_pt bundle;
	service_listener_pt listener;
	filter_pt filter;
    array_list_pt retainedReferences;
};

typedef struct fw_serviceListener * fw_service_listener_pt;

struct fw_bundleListener {
	bundle_pt bundle;
	bundle_listener_pt listener;
};

typedef struct fw_bundleListener * fw_bundle_listener_pt;

struct fw_frameworkListener {
	bundle_pt bundle;
	framework_listener_pt listener;
};

typedef struct fw_frameworkListener * fw_framework_listener_pt;

enum event_type {
	FRAMEWORK_EVENT_TYPE,
	BUNDLE_EVENT_TYPE,
	EVENT_TYPE_SERVICE,
};

typedef enum event_type event_type_e;

struct request {
	event_type_e type;
	array_list_pt listeners;

	int eventType;
	long bundleId;
	char* bundleSymbolicName;
	celix_status_t errorCode;
	char *error;

	char *filter;
};

typedef struct request *request_pt;

framework_logger_pt logger;

//TODO introduce a counter + mutex to control the freeing of the logger when mutiple threads are running a framework.
static celix_thread_once_t loggerInit = CELIX_THREAD_ONCE_INIT;
static void framework_loggerInit(void) {
    logger = malloc(sizeof(*logger));
    logger->logFunction = frameworkLogger_log;
}

#ifdef _WIN32
    #define handle_t HMODULE
    #define fw_openLibrary(path) LoadLibrary(path)
    #define fw_closeLibrary(handle) FreeLibrary(handle)

    #define fw_getSymbol(handle, name) GetProcAddress(handle, name)

    #define fw_getLastError() GetLastError()

    HMODULE fw_getCurrentModule() {
        HMODULE hModule = NULL;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)fw_getCurrentModule, &hModule);
        return hModule;
    }
#else
    #define handle_t void *
    #define fw_openLibrary(path) dlopen(path, RTLD_LAZY|RTLD_LOCAL)
    #define fw_closeLibrary(handle) dlclose(handle)
    #define fw_getSymbol(handle, name) dlsym(handle, name)
    #define fw_getLastError() dlerror()
#endif

celix_status_t framework_create(framework_pt *framework, properties_pt config) {
    celix_status_t status = CELIX_SUCCESS;

    logger = hashMap_get(config, "logger");
    if (logger == NULL) {
        celixThread_once(&loggerInit, framework_loggerInit);
    }

    *framework = (framework_pt) malloc(sizeof(**framework));
    if (*framework != NULL) {
        status = CELIX_DO_IF(status, celixThreadCondition_init(&(*framework)->condition, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->mutex, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->installedBundleMapLock, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->bundleLock, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->installRequestLock, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->dispatcherLock, NULL));
        status = CELIX_DO_IF(status, celixThreadMutex_create(&(*framework)->bundleListenerLock, NULL));
        status = CELIX_DO_IF(status, celixThreadCondition_init(&(*framework)->dispatcher, NULL));
        if (status == CELIX_SUCCESS) {
            (*framework)->bundle = NULL;
            (*framework)->installedBundleMap = NULL;
            (*framework)->registry = NULL;
            (*framework)->interrupted = false;
            (*framework)->shutdown = false;
            (*framework)->globalLockWaitersList = NULL;
            (*framework)->globalLockCount = 0;
            (*framework)->globalLockThread = celix_thread_default;
            (*framework)->nextBundleId = 1l;
            (*framework)->cache = NULL;
            (*framework)->installRequestMap = hashMap_create(utils_stringHash, utils_stringHash, utils_stringEquals, utils_stringEquals);
            (*framework)->serviceListeners = NULL;
            (*framework)->bundleListeners = NULL;
            (*framework)->frameworkListeners = NULL;
            (*framework)->requests = NULL;
            (*framework)->configurationMap = config;
            (*framework)->logger = logger;


            status = CELIX_DO_IF(status, bundle_create(&(*framework)->bundle));
            status = CELIX_DO_IF(status, arrayList_create(&(*framework)->globalLockWaitersList));
            status = CELIX_DO_IF(status, bundle_setFramework((*framework)->bundle, (*framework)));
            if (status == CELIX_SUCCESS) {
                //
            } else {
                status = CELIX_FRAMEWORK_EXCEPTION;
                fw_logCode((*framework)->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not create framework");
            }
        } else {
            status = CELIX_FRAMEWORK_EXCEPTION;
            fw_logCode((*framework)->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not create framework");
        }
    } else {
        status = CELIX_FRAMEWORK_EXCEPTION;
        fw_logCode(logger, OSGI_FRAMEWORK_LOG_ERROR, CELIX_ENOMEM, "Could not create framework");
    }

    return status;
}

celix_status_t framework_destroy(framework_pt framework) {
    celix_status_t status = CELIX_SUCCESS;

    celixThreadMutex_lock(&framework->installedBundleMapLock);

    if (framework->installedBundleMap != NULL) {
        hash_map_iterator_pt iterator = hashMapIterator_create(framework->installedBundleMap);
        while (hashMapIterator_hasNext(iterator)) {
            hash_map_entry_pt entry = hashMapIterator_nextEntry(iterator);
            bundle_pt bundle = (bundle_pt) hashMapEntry_getValue(entry);
            char * key = hashMapEntry_getKey(entry);
            bundle_archive_pt archive = NULL;

            bool systemBundle = false;
            bundle_isSystemBundle(bundle, &systemBundle);
            if (systemBundle) {
                bundle_context_pt context = NULL;
                bundle_getContext(framework->bundle, &context);
                bundleContext_destroy(context);
            }

            if (bundle_getArchive(bundle, &archive) == CELIX_SUCCESS) {
                if (!systemBundle) {
                    bundle_revision_pt revision = NULL;
                    array_list_pt handles = NULL;
                    status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
                    status = CELIX_DO_IF(status, bundleRevision_getHandles(revision, &handles));
                    for (int i = arrayList_size(handles) - 1; i >= 0; i--) {
                        void *handle = arrayList_get(handles, i);
                        fw_closeLibrary(handle);
                    }
                }

                bundleArchive_destroy(archive);
            }
            bundle_destroy(bundle);
            hashMapIterator_remove(iterator);
            free(key);
        }
        hashMapIterator_destroy(iterator);
    }

    celixThreadMutex_unlock(&framework->installedBundleMapLock);

	hashMap_destroy(framework->installRequestMap, false, false);

	serviceRegistry_destroy(framework->registry);

	arrayList_destroy(framework->globalLockWaitersList);

    if (framework->serviceListeners != NULL) {
        arrayList_destroy(framework->serviceListeners);
    }
    if (framework->bundleListeners) {
        arrayList_destroy(framework->bundleListeners);
    }
    if (framework->frameworkListeners) {
        arrayList_destroy(framework->frameworkListeners);
    }

	if(framework->requests){
	    int i;
	    for (i = 0; i < arrayList_size(framework->requests); i++) {
	        request_pt request = arrayList_get(framework->requests, i);
	        free(request);
	    }
	    arrayList_destroy(framework->requests);
	}
	if(framework->installedBundleMap!=NULL){
		hashMap_destroy(framework->installedBundleMap, true, false);
	}

	bundleCache_destroy(&framework->cache);

	celixThreadCondition_destroy(&framework->dispatcher);
	celixThreadMutex_destroy(&framework->bundleListenerLock);
	celixThreadMutex_destroy(&framework->dispatcherLock);
	celixThreadMutex_destroy(&framework->installRequestLock);
	celixThreadMutex_destroy(&framework->bundleLock);
	celixThreadMutex_destroy(&framework->installedBundleMapLock);
	celixThreadMutex_destroy(&framework->mutex);
	celixThreadCondition_destroy(&framework->condition);

    logger = hashMap_get(framework->configurationMap, "logger");
    if (logger == NULL) {
        free(framework->logger);
    }

    properties_destroy(framework->configurationMap);

    free(framework);

	return status;
}

celix_status_t fw_init(framework_pt framework) {
	bundle_state_e state;
	char *location = NULL;
	module_pt module = NULL;
	linked_list_pt wires = NULL;
	array_list_pt archives = NULL;
	bundle_archive_pt archive = NULL;

	celix_status_t status = CELIX_SUCCESS;
	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, framework->bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, arrayList_create(&framework->serviceListeners));
	status = CELIX_DO_IF(status, arrayList_create(&framework->bundleListeners));
	status = CELIX_DO_IF(status, arrayList_create(&framework->frameworkListeners));
	status = CELIX_DO_IF(status, arrayList_create(&framework->requests));
	status = CELIX_DO_IF(status, celixThread_create(&framework->dispatcherThread, NULL, fw_eventDispatcher, framework));
	status = CELIX_DO_IF(status, bundle_getState(framework->bundle, &state));
	if (status == CELIX_SUCCESS) {
	    if ((state == OSGI_FRAMEWORK_BUNDLE_INSTALLED) || (state == OSGI_FRAMEWORK_BUNDLE_RESOLVED)) {
	        bundle_state_e state;
	        status = CELIX_DO_IF(status, bundleCache_create(framework->configurationMap,&framework->cache));
	        status = CELIX_DO_IF(status, bundle_getState(framework->bundle, &state));
	        if (status == CELIX_SUCCESS) {
	            if (state == OSGI_FRAMEWORK_BUNDLE_INSTALLED) {
	                char *clean = properties_get(framework->configurationMap, (char *) OSGI_FRAMEWORK_FRAMEWORK_STORAGE_CLEAN);
	                if (clean != NULL && (strcmp(clean, OSGI_FRAMEWORK_FRAMEWORK_STORAGE_CLEAN_ONFIRSTINIT) == 0)) {
	                    bundleCache_delete(framework->cache);
	                }
	            }
            }
        }
	}

	if (status == CELIX_SUCCESS) {
        /*create and store framework uuid*/
        char uuid[37];

	    uuid_t uid;
        uuid_generate(uid);
        uuid_unparse(uid, uuid);

        properties_set(framework->configurationMap, (char*) OSGI_FRAMEWORK_FRAMEWORK_UUID, uuid);

        framework->installedBundleMap = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);
	}

    status = CELIX_DO_IF(status, bundle_getArchive(framework->bundle, &archive));
    status = CELIX_DO_IF(status, bundleArchive_getLocation(archive, &location));
    if (status == CELIX_SUCCESS) {
        hashMap_put(framework->installedBundleMap, strdup(location), framework->bundle);
    }
    status = CELIX_DO_IF(status, bundle_getCurrentModule(framework->bundle, &module));
    if (status == CELIX_SUCCESS) {
        wires = resolver_resolve(module);
        if (wires != NULL) {
            framework_markResolvedModules(framework, wires);
        } else {
            status = CELIX_BUNDLE_EXCEPTION;
            fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Unresolved constraints in System Bundle");
        }
    }

    status = CELIX_DO_IF(status, bundleCache_getArchives(framework->cache, &archives));
    if (status == CELIX_SUCCESS) {
        unsigned int arcIdx;
        for (arcIdx = 0; arcIdx < arrayList_size(archives); arcIdx++) {
            bundle_archive_pt archive1 = (bundle_archive_pt) arrayList_get(archives, arcIdx);
            long id;
            bundle_state_e bundleState;
            bundleArchive_getId(archive1, &id);
            framework->nextBundleId = framework->nextBundleId > id + 1 ? framework->nextBundleId : id + 1;

            bundleArchive_getPersistentState(archive1, &bundleState);
            if (bundleState == OSGI_FRAMEWORK_BUNDLE_UNINSTALLED) {
                bundleArchive_closeAndDelete(archive1);
            } else {
                bundle_pt bundle = NULL;
                char *location1 = NULL;
                status = bundleArchive_getLocation(archive1, &location1);
                fw_installBundle2(framework, &bundle, id, location1, NULL, archive1);
            }
        }
        arrayList_destroy(archives);
    }

    status = CELIX_DO_IF(status, serviceRegistry_create(framework, fw_serviceChanged, &framework->registry));
    status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, framework->bundle, OSGI_FRAMEWORK_BUNDLE_STARTING));
    status = CELIX_DO_IF(status, celixThreadCondition_init(&framework->shutdownGate, NULL));

    bundle_context_pt context = NULL;
    status = CELIX_DO_IF(status, bundleContext_create(framework, framework->logger, framework->bundle, &context));
    status = CELIX_DO_IF(status, bundle_setContext(framework->bundle, context));
    if (status == CELIX_SUCCESS) {
        activator_pt activator = NULL;
        activator = (activator_pt) malloc((sizeof(*activator)));
        if (activator != NULL) {
            bundle_context_pt context = NULL;
            void * userData = NULL;

			create_function_pt create = NULL;
			start_function_pt start = (start_function_pt) frameworkActivator_start;
			stop_function_pt stop = (stop_function_pt) frameworkActivator_stop;
			destroy_function_pt destroy = (destroy_function_pt) frameworkActivator_destroy;

            activator->start = start;
            activator->stop = stop;
            activator->destroy = destroy;
            status = CELIX_DO_IF(status, bundle_setActivator(framework->bundle, activator));
            status = CELIX_DO_IF(status, bundle_getContext(framework->bundle, &context));

            if (status == CELIX_SUCCESS) {
                if (create != NULL) {
                    create(context, &userData);
                }
                activator->userData = userData;

                if (start != NULL) {
                    start(userData, context);
                }
            }
        } else {
            status = CELIX_ENOMEM;
        }
    }

    if (status != CELIX_SUCCESS) {
       fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not init framework");
    }

    framework_releaseBundleLock(framework, framework->bundle);

	return status;
}

celix_status_t framework_start(framework_pt framework) {
	celix_status_t status = CELIX_SUCCESS;
	bundle_state_e state = OSGI_FRAMEWORK_BUNDLE_UNKNOWN;

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, framework->bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, bundle_getState(framework->bundle, &state));
	if (status == CELIX_SUCCESS) {
	    if ((state == OSGI_FRAMEWORK_BUNDLE_INSTALLED) || (state == OSGI_FRAMEWORK_BUNDLE_RESOLVED)) {
	        status = CELIX_DO_IF(status, fw_init(framework));
        }
	}

	status = CELIX_DO_IF(status, bundle_getState(framework->bundle, &state));
	if (status == CELIX_SUCCESS) {
	    if (state == OSGI_FRAMEWORK_BUNDLE_STARTING) {
	        status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, framework->bundle, OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	    }

	    framework_releaseBundleLock(framework, framework->bundle);
	}

	status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_STARTED, framework->bundle));
	status = CELIX_DO_IF(status, fw_fireFrameworkEvent(framework, OSGI_FRAMEWORK_EVENT_STARTED, framework->bundle, 0));

	if (status != CELIX_SUCCESS) {
       status = CELIX_BUNDLE_EXCEPTION;
       fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not start framework");
       fw_fireFrameworkEvent(framework, OSGI_FRAMEWORK_EVENT_ERROR, framework->bundle, status);
    }

	return status;
}

void framework_stop(framework_pt framework) {
	fw_stopBundle(framework, framework->bundle, true);
}

celix_status_t fw_getProperty(framework_pt framework, const char *name, char **value) {
	celix_status_t status = CELIX_SUCCESS;

	if (framework == NULL || name == NULL || *value != NULL) {
		status = CELIX_ILLEGAL_ARGUMENT;
		fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Missing required arguments");
	} else {
		if (framework->configurationMap != NULL) {
			*value = properties_get(framework->configurationMap, (char *) name);
		}
		if (*value == NULL) {
			*value = getenv(name);
		}
	}

	return status;
}

celix_status_t fw_installBundle(framework_pt framework, bundle_pt * bundle, char * location, char *inputFile) {
	return fw_installBundle2(framework, bundle, -1, location, inputFile, NULL);
}

celix_status_t fw_installBundle2(framework_pt framework, bundle_pt * bundle, long id, char * location, char *inputFile, bundle_archive_pt archive) {
    celix_status_t status = CELIX_SUCCESS;
//    bundle_archive_pt bundle_archive = NULL;
    bundle_state_e state = OSGI_FRAMEWORK_BUNDLE_UNKNOWN;
  	bool locked;

  	status = CELIX_DO_IF(status, framework_acquireInstallLock(framework, location));
  	status = CELIX_DO_IF(status, bundle_getState(framework->bundle, &state));
  	if (status == CELIX_SUCCESS) {
        if (state == OSGI_FRAMEWORK_BUNDLE_STOPPING || state == OSGI_FRAMEWORK_BUNDLE_UNINSTALLED) {
            fw_log(framework->logger, OSGI_FRAMEWORK_LOG_INFO,  "The framework is being shutdown");
            status = CELIX_DO_IF(status, framework_releaseInstallLock(framework, location));
            status = CELIX_FRAMEWORK_SHUTDOWN;
        }
  	}

    if (status == CELIX_SUCCESS) {
        *bundle = framework_getBundle(framework, location);
        if (*bundle != NULL) {
            framework_releaseInstallLock(framework, location);
            return CELIX_SUCCESS;
        }

        if (archive == NULL) {
            id = framework_getNextBundleId(framework);

            status = CELIX_DO_IF(status, bundleCache_createArchive(framework->cache, id, location, inputFile, &archive));

            if (status != CELIX_SUCCESS) {
            	bundleArchive_destroy(archive);
            }
        } else {
            // purge revision
            // multiple revisions not yet implemented
        }

        if (status == CELIX_SUCCESS) {
            locked = framework_acquireGlobalLock(framework);
            if (!locked) {
                status = CELIX_BUNDLE_EXCEPTION;
            } else {
                status = CELIX_DO_IF(status, bundle_createFromArchive(bundle, framework, archive));

                framework_releaseGlobalLock(framework);
                if (status == CELIX_SUCCESS) {
                    celixThreadMutex_lock(&framework->installedBundleMapLock);
                    hashMap_put(framework->installedBundleMap, strdup(location), *bundle);
                    celixThreadMutex_unlock(&framework->installedBundleMapLock);

                } else {
                    status = CELIX_BUNDLE_EXCEPTION;
                    status = CELIX_DO_IF(status, bundleArchive_closeAndDelete(archive));
                }
            }
        }
    }

    framework_releaseInstallLock(framework, location);

    if (status != CELIX_SUCCESS) {
    	fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not install bundle");
    } else {
        status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_INSTALLED, *bundle));
    }

  	return status;
}

celix_status_t framework_getBundleEntry(framework_pt framework, bundle_pt bundle, char *name, char **entry) {
	celix_status_t status = CELIX_SUCCESS;

	bundle_revision_pt revision;
	bundle_archive_pt archive = NULL;
    char *root;

	status = CELIX_DO_IF(status, bundle_getArchive(bundle, &archive));
    status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
    status = CELIX_DO_IF(status, bundleRevision_getRoot(revision, &root));
    if (status == CELIX_SUCCESS) {
        if ((strlen(name) > 0) && (name[0] == '/')) {
            name++;
        }
        char e[strlen(name) + strlen(root) + 2];
        strcpy(e, root);
        strcat(e, "/");
        strcat(e, name);

        struct stat info;

        if (stat(e, &info) == 0) {
            (*entry) = strdup(e);
        } else {
            (*entry) = NULL;
        }
    }

	return status;
}

celix_status_t fw_startBundle(framework_pt framework, bundle_pt bundle, int options) {
	celix_status_t status = CELIX_SUCCESS;

	linked_list_pt wires = NULL;
	bundle_context_pt context = NULL;
	bundle_state_e state;
	module_pt module = NULL;
	activator_pt activator = NULL;
	char *error = NULL;
	char *name = NULL;

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, bundle_getState(bundle, &state));

	if (status == CELIX_SUCCESS) {
	    switch (state) {
            case OSGI_FRAMEWORK_BUNDLE_UNKNOWN:
                error = "state is unknown";
                status = CELIX_ILLEGAL_STATE;
                break;
            case OSGI_FRAMEWORK_BUNDLE_UNINSTALLED:
                error = "bundle is uninstalled";
                status = CELIX_ILLEGAL_STATE;
                break;
            case OSGI_FRAMEWORK_BUNDLE_STARTING:
                error = "bundle is starting";
                status = CELIX_BUNDLE_EXCEPTION;
                break;
            case OSGI_FRAMEWORK_BUNDLE_STOPPING:
                error = "bundle is stopping";
                status = CELIX_BUNDLE_EXCEPTION;
                break;
            case OSGI_FRAMEWORK_BUNDLE_ACTIVE:
                break;
            case OSGI_FRAMEWORK_BUNDLE_INSTALLED:
                bundle_getCurrentModule(bundle, &module);
                module_getSymbolicName(module, &name);
                if (!module_isResolved(module)) {
                    wires = resolver_resolve(module);
                    if (wires == NULL) {
                        framework_releaseBundleLock(framework, bundle);
                        return CELIX_BUNDLE_EXCEPTION;
                    }
                    framework_markResolvedModules(framework, wires);

                }
                /* no break */
            case OSGI_FRAMEWORK_BUNDLE_RESOLVED:
                module = NULL;
                name = NULL;
                bundle_getCurrentModule(bundle, &module);
                module_getSymbolicName(module, &name);
                status = CELIX_DO_IF(status, bundleContext_create(framework, framework->logger, bundle, &context));
                status = CELIX_DO_IF(status, bundle_setContext(bundle, context));

                if (status == CELIX_SUCCESS) {
                    activator = (activator_pt) malloc((sizeof(*activator)));
                    if (activator == NULL) {
                        status = CELIX_ENOMEM;
                    } else {
                        void * userData = NULL;
                        bundle_context_pt context;
                        create_function_pt create = (create_function_pt) fw_getSymbol((handle_t) bundle_getHandle(bundle), OSGI_FRAMEWORK_BUNDLE_ACTIVATOR_CREATE);
                        start_function_pt start = (start_function_pt) fw_getSymbol((handle_t) bundle_getHandle(bundle), OSGI_FRAMEWORK_BUNDLE_ACTIVATOR_START);
                        stop_function_pt stop = (stop_function_pt) fw_getSymbol((handle_t) bundle_getHandle(bundle), OSGI_FRAMEWORK_BUNDLE_ACTIVATOR_STOP);
                        destroy_function_pt destroy = (destroy_function_pt) fw_getSymbol((handle_t) bundle_getHandle(bundle), OSGI_FRAMEWORK_BUNDLE_ACTIVATOR_DESTROY);

                        activator->start = start;
                        activator->stop = stop;
                        activator->destroy = destroy;
                        status = CELIX_DO_IF(status, bundle_setActivator(bundle, activator));

                        status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_STARTING));
                        status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_STARTING, bundle));

                        status = CELIX_DO_IF(status, bundle_getContext(bundle, &context));

                        if (status == CELIX_SUCCESS) {
                            if (create != NULL) {
                                status = CELIX_DO_IF(status, create(context, &userData));
                                if (status == CELIX_SUCCESS) {
                                    activator->userData = userData;
                                }
                            }
                        }
                        if (status == CELIX_SUCCESS) {
                            if (start != NULL) {

                                status = CELIX_DO_IF(status, start(userData, context));
                            }
                        }

                        status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_ACTIVE));
                        status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_STARTED, bundle));
                    }
                }

            break;
        }
	}

	framework_releaseBundleLock(framework, bundle);

	if (status != CELIX_SUCCESS) {
	    module_pt module = NULL;
	    char *symbolicName = NULL;
	    long id = 0;
	    bundle_getCurrentModule(bundle, &module);
	    module_getSymbolicName(module, &symbolicName);
	    bundle_getBundleId(bundle, &id);
	    if (error != NULL) {
	        fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not start bundle: %s [%ld]; cause: %s", symbolicName, id, error);
	    } else {
	        fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not start bundle: %s [%ld]", symbolicName, id);
	    }
	}

	return status;
}

celix_status_t framework_updateBundle(framework_pt framework, bundle_pt bundle, char *inputFile) {
	celix_status_t status = CELIX_SUCCESS;
	bundle_state_e oldState;
	char *location;
	bundle_archive_pt archive = NULL;
	char *error = NULL;

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, bundle_getState(bundle, &oldState));
	if (status == CELIX_SUCCESS) {
        if (oldState == OSGI_FRAMEWORK_BUNDLE_ACTIVE) {
            fw_stopBundle(framework, bundle, false);
        }
	}
	status = CELIX_DO_IF(status, bundle_getArchive(bundle, &archive));
	status = CELIX_DO_IF(status, bundleArchive_getLocation(archive, &location));

	if (status == CELIX_SUCCESS) {
	    bool locked = framework_acquireGlobalLock(framework);
	    if (!locked) {
	        status = CELIX_BUNDLE_EXCEPTION;
	        error = "Unable to acquire the global lock to update the bundle";
	    }
	}

	status = CELIX_DO_IF(status, bundle_revise(bundle, location, inputFile));
	status = CELIX_DO_IF(status, framework_releaseGlobalLock(framework));

	status = CELIX_DO_IF(status, bundleArchive_setLastModified(archive, time(NULL)));
	status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED));

	// TODO Unload all libraries for transition to unresolved
	bundle_revision_pt revision = NULL;
	array_list_pt handles = NULL;
	status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
    status = CELIX_DO_IF(status, bundleRevision_getHandles(revision, &handles));
    for (int i = arrayList_size(handles) - 1; i >= 0; i--) {
        void *handle = arrayList_get(handles, i);
        fw_closeLibrary(handle);
    }


	status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_UNRESOLVED, bundle));
	status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_UPDATED, bundle));

    // Refresh packages?

	if (status == CELIX_SUCCESS) {
	    if (oldState == OSGI_FRAMEWORK_BUNDLE_ACTIVE) {
	        status = CELIX_DO_IF(status, fw_startBundle(framework, bundle, 1));
	    }
	}

	framework_releaseBundleLock(framework, bundle);

	if (status != CELIX_SUCCESS) {
	    module_pt module = NULL;
        char *symbolicName = NULL;
        long id = 0;
        bundle_getCurrentModule(bundle, &module);
        module_getSymbolicName(module, &symbolicName);
        bundle_getBundleId(bundle, &id);
        if (error != NULL) {
            fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Cannot update bundle: %s [%ld]; cause: %s", symbolicName, id, error);
        } else {
            fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Cannot update bundle: %s [%ld]", symbolicName, id);
        }
	}

	return status;
}

celix_status_t fw_stopBundle(framework_pt framework, bundle_pt bundle, bool record) {
	celix_status_t status = CELIX_SUCCESS;
	bundle_state_e state;
    activator_pt activator = NULL;
    bundle_context_pt context = NULL;
    bool wasActive = false;
    long id = 0;
    char *error = NULL;

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));

	if (record) {
	    status = CELIX_DO_IF(status, bundle_setPersistentStateInactive(bundle));
    }

	status = CELIX_DO_IF(status, bundle_getState(bundle, &state));
	if (status == CELIX_SUCCESS) {
	    switch (state) {
            case OSGI_FRAMEWORK_BUNDLE_UNKNOWN:
                status = CELIX_ILLEGAL_STATE;
                error = "state is unknown";
                break;
            case OSGI_FRAMEWORK_BUNDLE_UNINSTALLED:
                status = CELIX_ILLEGAL_STATE;
                error = "bundle is uninstalled";
                break;
            case OSGI_FRAMEWORK_BUNDLE_STARTING:
                status = CELIX_BUNDLE_EXCEPTION;
                error = "bundle is starting";
                break;
            case OSGI_FRAMEWORK_BUNDLE_STOPPING:
                status = CELIX_BUNDLE_EXCEPTION;
                error = "bundle is stopping";
                break;
            case OSGI_FRAMEWORK_BUNDLE_INSTALLED:
            case OSGI_FRAMEWORK_BUNDLE_RESOLVED:
                break;
            case OSGI_FRAMEWORK_BUNDLE_ACTIVE:
                wasActive = true;
                break;
        }
	}


	status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_STOPPING));
	status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_STOPPING, bundle));
    status = CELIX_DO_IF(status, bundle_getBundleId(bundle, &id));
	if (status == CELIX_SUCCESS) {
	    if (wasActive || (id == 0)) {
	        activator = bundle_getActivator(bundle);

	        status = CELIX_DO_IF(status, bundle_getContext(bundle, &context));
	        if (status == CELIX_SUCCESS) {
                if (activator->stop != NULL) {
                    status = CELIX_DO_IF(status, activator->stop(activator->userData, context));
                }
	        }
            if (status == CELIX_SUCCESS) {
                if (activator->destroy != NULL) {
                    status = CELIX_DO_IF(status, activator->destroy(activator->userData, context));
                }
	        }

            if (id != 0) {
                status = CELIX_DO_IF(status, serviceRegistry_clearServiceRegistrations(framework->registry, bundle));
                if (status == CELIX_SUCCESS) {
                    module_pt module = NULL;
                    char *symbolicName = NULL;
                    long id = 0;
                    bundle_getCurrentModule(bundle, &module);
                    module_getSymbolicName(module, &symbolicName);
                    bundle_getBundleId(bundle, &id);

                    serviceRegistry_clearReferencesFor(framework->registry, bundle);
                }
                // #TODO remove listeners for bundle

                if (context != NULL) {
                    status = CELIX_DO_IF(status, bundleContext_destroy(context));
                    status = CELIX_DO_IF(status, bundle_setContext(bundle, NULL));
                }

                status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_RESOLVED));
            }
	    }

	    if (activator != NULL) {
	        bundle_setActivator(bundle, NULL);
	        free(activator);
	    }
	}

	framework_releaseBundleLock(framework, bundle);

	if (status != CELIX_SUCCESS) {
	    module_pt module = NULL;
        char *symbolicName = NULL;
        long id = 0;
        bundle_getCurrentModule(bundle, &module);
        module_getSymbolicName(module, &symbolicName);
        bundle_getBundleId(bundle, &id);
        if (error != NULL) {
            fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Cannot stop bundle: %s [%ld]; cause: %s", symbolicName, id, error);
        } else {
            fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Cannot stop bundle: %s [%ld]", symbolicName, id);
        }
 	} else {
        fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_STOPPED, bundle);
 	}

	return status;
}

celix_status_t fw_uninstallBundle(framework_pt framework, bundle_pt bundle) {
    celix_status_t status = CELIX_SUCCESS;
    bool locked;
    bundle_archive_pt archive = NULL;
    char * location = NULL;
    bundle_pt target = NULL;
    char *error = NULL;

    status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE|OSGI_FRAMEWORK_BUNDLE_STOPPING));
    status = CELIX_DO_IF(status, fw_stopBundle(framework, bundle, true));
    if (status == CELIX_SUCCESS) {
        locked = framework_acquireGlobalLock(framework);
        if (!locked) {
            status = CELIX_ILLEGAL_STATE;
            error = "Unable to acquire the global lock to uninstall the bundle";
        }
    }

    status = CELIX_DO_IF(status, bundle_getArchive(bundle, &archive));
    status = CELIX_DO_IF(status, bundleArchive_getLocation(archive, &location));
    if (status == CELIX_SUCCESS) {

        celixThreadMutex_lock(&framework->installedBundleMapLock);

        hash_map_entry_pt entry = hashMap_getEntry(framework->installedBundleMap, location);
        char* entryLocation = hashMapEntry_getKey(entry);

        target = (bundle_pt) hashMap_remove(framework->installedBundleMap, location);

        free(entryLocation);
        if (target != NULL) {
            status = CELIX_DO_IF(status, bundle_setPersistentStateUninstalled(target));
            // fw_rememberUninstalledBundle(framework, target);
        }
        celixThreadMutex_unlock(&framework->installedBundleMapLock);

    }

    framework_releaseGlobalLock(framework);

    if (status == CELIX_SUCCESS) {
        if (target == NULL) {
            fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, "Could not remove bundle from installed map");
        }
    }

    status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED));

    // TODO Unload all libraries for transition to unresolved
    bundle_revision_pt revision = NULL;
	array_list_pt handles = NULL;
	status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
	status = CELIX_DO_IF(status, bundleRevision_getHandles(revision, &handles));
	for (int i = arrayList_size(handles) - 1; i >= 0; i--) {
		void *handle = arrayList_get(handles, i);
		fw_closeLibrary(handle);
	}

    status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_UNRESOLVED, bundle));

    status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_UNINSTALLED));
    status = CELIX_DO_IF(status, bundleArchive_setLastModified(archive, time(NULL)));

    framework_releaseBundleLock(framework, bundle);

    status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_UNINSTALLED, bundle));

    if (status == CELIX_SUCCESS) {
        locked = framework_acquireGlobalLock(framework);
        if (locked) {
            bundle_pt bundles[] = { bundle };
            celix_status_t refreshStatus = fw_refreshBundles(framework, bundles, 1);
            if (refreshStatus != CELIX_SUCCESS) {
                printf("Could not refresh bundle");
            } else {
                bundleArchive_destroy(archive);
                status = CELIX_DO_IF(status, bundle_destroy(bundle));
            }

            status = CELIX_DO_IF(status, framework_releaseGlobalLock(framework));
        }
    }


    if (status != CELIX_SUCCESS) {
//        module_pt module = NULL;
//        char *symbolicName = NULL;
//        long id = 0;
//        bundle_getCurrentModule(bundle, &module);
//        module_getSymbolicName(module, &symbolicName);
//        bundle_getBundleId(bundle, &id);

        framework_logIfError(framework->logger, status, error, "Cannot uninstall bundle");
    }

    return status;
}

celix_status_t fw_refreshBundles(framework_pt framework, bundle_pt bundles[], int size) {
    celix_status_t status = CELIX_SUCCESS;

    bool locked = framework_acquireGlobalLock(framework);
    if (!locked) {
        framework_releaseGlobalLock(framework);
        status = CELIX_ILLEGAL_STATE;
    } else {
		hash_map_values_pt values;
        bundle_pt *newTargets;
        unsigned int nrofvalues;
		bool restart = false;
        hash_map_pt map = hashMap_create(NULL, NULL, NULL, NULL);
        int targetIdx = 0;
        for (targetIdx = 0; targetIdx < size; targetIdx++) {
            bundle_pt bundle = bundles[targetIdx];
            hashMap_put(map, bundle, bundle);
            fw_populateDependentGraph(framework, bundle, &map);
        }
        values = hashMapValues_create(map);
        hashMapValues_toArray(values, (void ***) &newTargets, &nrofvalues);
        hashMapValues_destroy(values);

        hashMap_destroy(map, false, false);

        if (newTargets != NULL) {
            int i = 0;
			struct fw_refreshHelper * helpers;
            for (i = 0; i < nrofvalues && !restart; i++) {
                bundle_pt bundle = (bundle_pt) newTargets[i];
                if (framework->bundle == bundle) {
                    restart = true;
                }
            }

            helpers = (struct fw_refreshHelper * )malloc(nrofvalues * sizeof(struct fw_refreshHelper));
            for (i = 0; i < nrofvalues && !restart; i++) {
                bundle_pt bundle = (bundle_pt) newTargets[i];
                helpers[i].framework = framework;
                helpers[i].bundle = bundle;
                helpers[i].oldState = OSGI_FRAMEWORK_BUNDLE_INSTALLED;
            }

            for (i = 0; i < nrofvalues; i++) {
                struct fw_refreshHelper helper = helpers[i];
                fw_refreshHelper_stop(&helper);
                fw_refreshHelper_refreshOrRemove(&helper);
            }

            for (i = 0; i < nrofvalues; i++) {
                struct fw_refreshHelper helper = helpers[i];
                fw_refreshHelper_restart(&helper);
            }

            if (restart) {
                bundle_update(framework->bundle, NULL);
            }
			free(helpers);
			free(newTargets);
        }

        framework_releaseGlobalLock(framework);
    }

    framework_logIfError(framework->logger, status, NULL, "Cannot refresh bundles");

    return status;
}

celix_status_t fw_refreshBundle(framework_pt framework, bundle_pt bundle) {
    celix_status_t status = CELIX_SUCCESS;
    bundle_state_e state;

    status = framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED | OSGI_FRAMEWORK_BUNDLE_RESOLVED);
    if (status != CELIX_SUCCESS) {
        printf("Cannot refresh bundle");
        framework_releaseBundleLock(framework, bundle);
    } else {
    	bool fire;
		bundle_getState(bundle, &state);
        fire = (state != OSGI_FRAMEWORK_BUNDLE_INSTALLED);
        bundle_refresh(bundle);

        if (fire) {
            framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED);
            fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_UNRESOLVED, bundle);
        }

        framework_releaseBundleLock(framework, bundle);
    }

    framework_logIfError(framework->logger, status, NULL, "Cannot refresh bundle");

    return status;
}

celix_status_t fw_refreshHelper_stop(struct fw_refreshHelper * refreshHelper) {
	bundle_state_e state;
	bundle_getState(refreshHelper->bundle, &state);
    if (state == OSGI_FRAMEWORK_BUNDLE_ACTIVE) {
        refreshHelper->oldState = OSGI_FRAMEWORK_BUNDLE_ACTIVE;
        fw_stopBundle(refreshHelper->framework, refreshHelper->bundle, false);
    }

    return CELIX_SUCCESS;
}

celix_status_t fw_refreshHelper_refreshOrRemove(struct fw_refreshHelper * refreshHelper) {
	bundle_state_e state;
	bundle_getState(refreshHelper->bundle, &state);
    if (state == OSGI_FRAMEWORK_BUNDLE_UNINSTALLED) {
        bundle_closeAndDelete(refreshHelper->bundle);
        refreshHelper->bundle = NULL;
    } else {
        fw_refreshBundle(refreshHelper->framework, refreshHelper->bundle);
    }
    return CELIX_SUCCESS;
}

celix_status_t fw_refreshHelper_restart(struct fw_refreshHelper * refreshHelper) {
    if ((refreshHelper->bundle != NULL) && (refreshHelper->oldState == OSGI_FRAMEWORK_BUNDLE_ACTIVE)) {
        fw_startBundle(refreshHelper->framework, refreshHelper->bundle, 0);
    }
    return CELIX_SUCCESS;
}

celix_status_t fw_getDependentBundles(framework_pt framework, bundle_pt exporter, array_list_pt *list) {
    celix_status_t status = CELIX_SUCCESS;

    if (*list == NULL && exporter != NULL && framework != NULL) {
		array_list_pt modules;
		unsigned int modIdx = 0;
        arrayList_create(list);

        modules = bundle_getModules(exporter);
        for (modIdx = 0; modIdx < arrayList_size(modules); modIdx++) {
            module_pt module = (module_pt) arrayList_get(modules, modIdx);
            array_list_pt dependents = module_getDependents(module);
            unsigned int depIdx = 0;
            for (depIdx = 0; (dependents != NULL) && (depIdx < arrayList_size(dependents)); depIdx++) {
                module_pt dependent = (module_pt) arrayList_get(dependents, depIdx);
                arrayList_add(*list, module_getBundle(dependent));
            }
            arrayList_destroy(dependents);
        }
    } else {
        status = CELIX_ILLEGAL_ARGUMENT;
    }

    framework_logIfError(framework->logger, status, NULL, "Cannot get dependent bundles");

    return status;
}

celix_status_t fw_populateDependentGraph(framework_pt framework, bundle_pt exporter, hash_map_pt *map) {
    celix_status_t status = CELIX_SUCCESS;

    if (exporter != NULL && framework != NULL) {
        array_list_pt dependents = NULL;
        if ((status = fw_getDependentBundles(framework, exporter, &dependents)) == CELIX_SUCCESS) {
            unsigned int depIdx = 0;
            for (depIdx = 0; (dependents != NULL) && (depIdx < arrayList_size(dependents)); depIdx++) {
                if (!hashMap_containsKey(*map, arrayList_get(dependents, depIdx))) {
                    hashMap_put(*map, arrayList_get(dependents, depIdx), arrayList_get(dependents, depIdx));
                    fw_populateDependentGraph(framework, (bundle_pt) arrayList_get(dependents, depIdx), map);
                }
            }
            arrayList_destroy(dependents);
        }
    } else {
        status = CELIX_ILLEGAL_ARGUMENT;
    }

    framework_logIfError(framework->logger, status, NULL, "Cannot populate dependent graph");

    return status;
}

celix_status_t fw_registerService(framework_pt framework, service_registration_pt *registration, bundle_pt bundle, char * serviceName, void * svcObj, properties_pt properties) {
	celix_status_t status = CELIX_SUCCESS;
	char *error = NULL;
	if (serviceName == NULL || svcObj == NULL) {
	    status = CELIX_ILLEGAL_ARGUMENT;
	    error = "ServiceName and SvcObj cannot be null";
	}

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, serviceRegistry_registerService(framework->registry, bundle, serviceName, svcObj, properties, registration));
	bool res = framework_releaseBundleLock(framework, bundle);
	if (!res) {
	    status = CELIX_ILLEGAL_STATE;
	    error = "Could not release bundle lock";
	}

	if (status == CELIX_SUCCESS) {
	    // If this is a listener hook, invoke the callback with all current listeners
        if (strcmp(serviceName, OSGI_FRAMEWORK_LISTENER_HOOK_SERVICE_NAME) == 0) {
            unsigned int i;
            array_list_pt infos = NULL;
            service_reference_pt ref = NULL;
            listener_hook_service_pt hook = NULL;

            status = CELIX_DO_IF(status, arrayList_create(&infos));

            if (status == CELIX_SUCCESS) {
                celix_status_t subs = CELIX_SUCCESS;

                for (i = 0; i < arrayList_size(framework->serviceListeners); i++) {
                    fw_service_listener_pt listener =(fw_service_listener_pt) arrayList_get(framework->serviceListeners, i);
                    bundle_context_pt context = NULL;
                    listener_hook_info_pt info = NULL;
                    bundle_context_pt lContext = NULL;

                    subs = CELIX_DO_IF(subs, bundle_getContext(bundle, &context));
                    if (subs == CELIX_SUCCESS) {
                        info = (listener_hook_info_pt) malloc(sizeof(*info));
                        if (info == NULL) {
                            subs = CELIX_ENOMEM;
                        }
                    }

                    subs = CELIX_DO_IF(subs, bundle_getContext(listener->bundle, &lContext));
                    if (subs == CELIX_SUCCESS) {
                        info->context = lContext;
                        info->removed = false;
                    }
                    subs = CELIX_DO_IF(subs, filter_getString(listener->filter, &info->filter));

                    if (subs == CELIX_SUCCESS) {
                        arrayList_add(infos, info);
                    }
                    if (subs != CELIX_SUCCESS) {
                        fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not pass all listeners to the hook: %s", serviceName);
                    }
                }

                status = CELIX_DO_IF(status, serviceRegistry_getServiceReference(framework->registry, framework->bundle,
                                                                                 *registration, &ref));
                status = CELIX_DO_IF(status, fw_getService(framework,framework->bundle, ref, (void **) &hook));
                if (status == CELIX_SUCCESS) {
                    hook->added(hook->handle, infos);
                }
                status = CELIX_DO_IF(status, serviceRegistry_ungetService(framework->registry, framework->bundle, ref, NULL));
                status = CELIX_DO_IF(status, serviceRegistry_ungetServiceReference(framework->registry, framework->bundle, ref));

                int i = 0;
                for (i = 0; i < arrayList_size(infos); i++) {
                    listener_hook_info_pt info = arrayList_get(infos, i);
                    free(info);
                }
                arrayList_destroy(infos);
             }
        }
	}

    framework_logIfError(framework->logger, status, error, "Cannot register service: %s", serviceName);

	return status;
}

celix_status_t fw_registerServiceFactory(framework_pt framework, service_registration_pt *registration, bundle_pt bundle, char * serviceName, service_factory_pt factory, properties_pt properties) {
    celix_status_t status = CELIX_SUCCESS;
    char *error = NULL;
	if (serviceName == NULL || factory == NULL) {
        status = CELIX_ILLEGAL_ARGUMENT;
        error = "Service name and factory cannot be null";
    }

	status = CELIX_DO_IF(status, framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_STARTING|OSGI_FRAMEWORK_BUNDLE_ACTIVE));
	status = CELIX_DO_IF(status, serviceRegistry_registerServiceFactory(framework->registry, bundle, serviceName, factory, properties, registration));
    if (!framework_releaseBundleLock(framework, bundle)) {
        status = CELIX_ILLEGAL_STATE;
        error = "Could not release bundle lock";
    }

    framework_logIfError(framework->logger, status, error, "Cannot register service factory: %s", serviceName);

    return CELIX_SUCCESS;
}

celix_status_t fw_getServiceReferences(framework_pt framework, array_list_pt *references, bundle_pt bundle, const char * serviceName, char * sfilter) {
    celix_status_t status = CELIX_SUCCESS;

	filter_pt filter = NULL;
	unsigned int refIdx = 0;

    if (sfilter != NULL) {
        filter = filter_create(sfilter);
	}

	status = CELIX_DO_IF(status, serviceRegistry_getServiceReferences(framework->registry, bundle, serviceName, filter, references));

	if (filter != NULL) {
		filter_destroy(filter);
	}

	if (status == CELIX_SUCCESS) {
        for (refIdx = 0; (*references != NULL) && refIdx < arrayList_size(*references); refIdx++) {
            service_reference_pt ref = (service_reference_pt) arrayList_get(*references, refIdx);
            service_registration_pt reg = NULL;
            char * serviceName;
            properties_pt props = NULL;
            status = CELIX_DO_IF(status, serviceReference_getServiceRegistration(ref, &reg));
            status = CELIX_DO_IF(status, serviceRegistration_getProperties(reg, &props));
            if (status == CELIX_SUCCESS) {
                serviceName = properties_get(props, (char *) OSGI_FRAMEWORK_OBJECTCLASS);
                if (!serviceReference_isAssignableTo(ref, bundle, serviceName)) {
                    arrayList_remove(*references, refIdx);
                    refIdx--;
                }
            }
        }
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to get service references");

	return status;
}

celix_status_t framework_ungetServiceReference(framework_pt framework, bundle_pt bundle, service_reference_pt reference) {
    return serviceRegistry_ungetServiceReference(framework->registry, bundle, reference);
}

celix_status_t fw_getService(framework_pt framework, bundle_pt bundle, service_reference_pt reference, void **service) {
	return serviceRegistry_getService(framework->registry, bundle, reference, service);
}

celix_status_t fw_getBundleRegisteredServices(framework_pt framework, bundle_pt bundle, array_list_pt *services) {
	return serviceRegistry_getRegisteredServices(framework->registry, bundle, services);
}

celix_status_t fw_getBundleServicesInUse(framework_pt framework, bundle_pt bundle, array_list_pt *services) {
	celix_status_t status = CELIX_SUCCESS;
	status = serviceRegistry_getServicesInUse(framework->registry, bundle, services);
	return status;
}

celix_status_t framework_ungetService(framework_pt framework, bundle_pt bundle, service_reference_pt reference, bool *result) {
	return serviceRegistry_ungetService(framework->registry, bundle, reference, result);
}

void fw_addServiceListener(framework_pt framework, bundle_pt bundle, service_listener_pt listener, char * sfilter) {
	array_list_pt listenerHooks = NULL;
	listener_hook_info_pt info;
	unsigned int i;

	fw_service_listener_pt fwListener = (fw_service_listener_pt) calloc(1, sizeof(*fwListener));
	bundle_context_pt context = NULL;

	fwListener->bundle = bundle;
    arrayList_create(&fwListener->retainedReferences);
	if (sfilter != NULL) {
		filter_pt filter = filter_create(sfilter);
		fwListener->filter = filter;
	} else {
		fwListener->filter = NULL;
	}
	fwListener->listener = listener;

	arrayList_add(framework->serviceListeners, fwListener);

	serviceRegistry_getListenerHooks(framework->registry, framework->bundle, &listenerHooks);

	info = (listener_hook_info_pt) malloc(sizeof(*info));

	bundle_getContext(bundle, &context);
	info->context = context;

	info->removed = false;
	info->filter = sfilter == NULL ? NULL : strdup(sfilter);

	for (i = 0; i < arrayList_size(listenerHooks); i++) {
		service_reference_pt ref = (service_reference_pt) arrayList_get(listenerHooks, i);
		listener_hook_service_pt hook = NULL;
		array_list_pt infos = NULL;
		bool ungetResult = false;

		fw_getService(framework, framework->bundle, ref, (void **) &hook);

		arrayList_create(&infos);
		arrayList_add(infos, info);
		hook->added(hook->handle, infos);
		serviceRegistry_ungetService(framework->registry, framework->bundle, ref, &ungetResult);
		serviceRegistry_ungetServiceReference(framework->registry, framework->bundle, ref);
		arrayList_destroy(infos);
	}

	if (info->filter != NULL) {
	    free(info->filter);
	}
	free(info);

	arrayList_destroy(listenerHooks);
}

void fw_removeServiceListener(framework_pt framework, bundle_pt bundle, service_listener_pt listener) {
	listener_hook_info_pt info = NULL;
	unsigned int i;
	fw_service_listener_pt element;

	bundle_context_pt context;
	bundle_getContext(bundle, &context);

	for (i = 0; i < arrayList_size(framework->serviceListeners); i++) {
		element = (fw_service_listener_pt) arrayList_get(framework->serviceListeners, i);
		if (element->listener == listener && element->bundle == bundle) {
			bundle_context_pt lContext = NULL;

			info = (listener_hook_info_pt) malloc(sizeof(*info));

			bundle_getContext(element->bundle, &lContext);
			info->context = lContext;

			// TODO Filter toString;
			filter_getString(element->filter, &info->filter);
			info->removed = true;

			arrayList_remove(framework->serviceListeners, i);
			i--;
            
            //unregistering retained service references. For these refs a unregister event will not be triggered.
            int k;
            int rSize = arrayList_size(element->retainedReferences);
            for (k = 0; k < rSize; k += 1) {
                service_reference_pt ref = arrayList_get(element->retainedReferences, k);
                if (ref != NULL) {
                    serviceRegistry_ungetServiceReference(framework->registry, element->bundle, ref); // decrease retain counter                                       
                } 
            }

			element->bundle = NULL;
			filter_destroy(element->filter);
            arrayList_destroy(element->retainedReferences);
			element->filter = NULL;
			element->listener = NULL;
			free(element);
			element = NULL;
			break;
		}
	}

	if (info != NULL) {
		unsigned int i;
		array_list_pt listenerHooks = NULL;
		serviceRegistry_getListenerHooks(framework->registry, framework->bundle, &listenerHooks);

		for (i = 0; i < arrayList_size(listenerHooks); i++) {
			service_reference_pt ref = (service_reference_pt) arrayList_get(listenerHooks, i);
			listener_hook_service_pt hook = NULL;
			array_list_pt infos = NULL;
			bool ungetResult;

			fw_getService(framework, framework->bundle, ref, (void **) &hook);

			arrayList_create(&infos);
			arrayList_add(infos, info);
			hook->removed(hook->handle, infos);
			serviceRegistry_ungetService(framework->registry, framework->bundle, ref, &ungetResult);
			serviceRegistry_ungetServiceReference(framework->registry, framework->bundle, ref);
			arrayList_destroy(infos);
		}

		arrayList_destroy(listenerHooks);
        free(info);
	}
}

celix_status_t fw_addBundleListener(framework_pt framework, bundle_pt bundle, bundle_listener_pt listener) {
	celix_status_t status = CELIX_SUCCESS;
	fw_bundle_listener_pt bundleListener = NULL;

	bundleListener = (fw_bundle_listener_pt) malloc(sizeof(*bundleListener));
	if (!bundleListener) {
		status = CELIX_ENOMEM;
	} else {
		bundleListener->listener = listener;
		bundleListener->bundle = bundle;

		if (celixThreadMutex_lock(&framework->bundleListenerLock) != CELIX_SUCCESS) {
			status = CELIX_FRAMEWORK_EXCEPTION;
		} else {
			arrayList_add(framework->bundleListeners, bundleListener);

			if (celixThreadMutex_unlock(&framework->bundleListenerLock)) {
				status = CELIX_FRAMEWORK_EXCEPTION;
			}
		}
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to add bundle listener");

	return status;
}

celix_status_t fw_removeBundleListener(framework_pt framework, bundle_pt bundle, bundle_listener_pt listener) {
	celix_status_t status = CELIX_SUCCESS;

	unsigned int i;
	fw_bundle_listener_pt bundleListener;

	if (celixThreadMutex_lock(&framework->bundleListenerLock) != CELIX_SUCCESS) {
		status = CELIX_FRAMEWORK_EXCEPTION;
	}
	else {
		for (i = 0; i < arrayList_size(framework->bundleListeners); i++) {
			bundleListener = (fw_bundle_listener_pt) arrayList_get(framework->bundleListeners, i);
			if (bundleListener->listener == listener && bundleListener->bundle == bundle) {
				arrayList_remove(framework->bundleListeners, i);

				bundleListener->bundle = NULL;
				bundleListener->listener = NULL;
				free(bundleListener);
			}
		}
		if (celixThreadMutex_unlock(&framework->bundleListenerLock)) {
			status = CELIX_FRAMEWORK_EXCEPTION;
		}
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to remove bundle listener");

	return status;
}

celix_status_t fw_addFrameworkListener(framework_pt framework, bundle_pt bundle, framework_listener_pt listener) {
	celix_status_t status = CELIX_SUCCESS;
	fw_framework_listener_pt frameworkListener = NULL;

	frameworkListener = (fw_framework_listener_pt) malloc(sizeof(*frameworkListener));
	if (!frameworkListener) {
		status = CELIX_ENOMEM;
	} else {
		frameworkListener->listener = listener;
		frameworkListener->bundle = bundle;

		arrayList_add(framework->frameworkListeners, frameworkListener);
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to add framework listener");

	return status;
}

celix_status_t fw_removeFrameworkListener(framework_pt framework, bundle_pt bundle, framework_listener_pt listener) {
	celix_status_t status = CELIX_SUCCESS;

	unsigned int i;
	fw_framework_listener_pt frameworkListener;

	for (i = 0; i < arrayList_size(framework->frameworkListeners); i++) {
		frameworkListener = (fw_framework_listener_pt) arrayList_get(framework->frameworkListeners, i);
		if (frameworkListener->listener == listener && frameworkListener->bundle == bundle) {
			arrayList_remove(framework->frameworkListeners, i);

			frameworkListener->bundle = NULL;
            frameworkListener->listener = NULL;
            free(frameworkListener);
		}
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to remove framework listener");

	return status;
}

void fw_serviceChanged(framework_pt framework, service_event_type_e eventType, service_registration_pt registration, properties_pt oldprops) {
    unsigned int i;
    fw_service_listener_pt element;

    if (arrayList_size(framework->serviceListeners) > 0) {
        for (i = 0; i < arrayList_size(framework->serviceListeners); i++) {
            int matched = 0;
            properties_pt props = NULL;
            bool matchResult = false;

            element = (fw_service_listener_pt) arrayList_get(framework->serviceListeners, i);
            serviceRegistration_getProperties(registration, &props);
            if (element->filter != NULL) {
                filter_match(element->filter, props, &matchResult);
            }
            matched = (element->filter == NULL) || matchResult;
            if (matched) {
                service_reference_pt reference = NULL;
                service_event_pt event;

                event = (service_event_pt) malloc(sizeof (*event));

                serviceRegistry_getServiceReference(framework->registry, element->bundle, registration, &reference);
                
                //NOTE: that you are never sure that the UNREGISTERED event will by handle by an service_listener. listener could be gone
                //Every reference retained is therefore stored and called when a service listener is removed from the framework.
                if (eventType == OSGI_FRAMEWORK_SERVICE_EVENT_REGISTERED) {
                    serviceRegistry_retainServiceReference(framework->registry, element->bundle, reference);
                    arrayList_add(element->retainedReferences, reference); //TODO improve by using set (or hashmap) instead of list
                }

                event->type = eventType;
                event->reference = reference;

                element->listener->serviceChanged(element->listener, event);

                serviceRegistry_ungetServiceReference(framework->registry, element->bundle, reference);
                
                if (eventType == OSGI_FRAMEWORK_SERVICE_EVENT_UNREGISTERING) {
                    arrayList_removeElement(element->retainedReferences, reference);
                    serviceRegistry_ungetServiceReference(framework->registry, element->bundle, reference); // decrease retain counter
                }
                
                free(event);

            } else if (eventType == OSGI_FRAMEWORK_SERVICE_EVENT_MODIFIED) {
                bool matchResult = false;
                int matched = 0;
                if (element->filter != NULL) {
                    filter_match(element->filter, oldprops, &matchResult);
                }
                matched = (element->filter == NULL) || matchResult;
                if (matched) {
                    service_reference_pt reference = NULL;
                    service_event_pt endmatch = (service_event_pt) malloc(sizeof (*endmatch));

                    serviceRegistry_getServiceReference(framework->registry, element->bundle, registration, &reference);

                    endmatch->reference = reference;
                    endmatch->type = OSGI_FRAMEWORK_SERVICE_EVENT_MODIFIED_ENDMATCH;
                    element->listener->serviceChanged(element->listener, endmatch);

                    serviceRegistry_ungetServiceReference(framework->registry, element->bundle, reference);
                    free(endmatch);

                }
            }
        }
    }

}

//celix_status_t fw_isServiceAssignable(framework_pt fw, bundle_pt requester, service_reference_pt reference, bool *assignable) {
//	celix_status_t status = CELIX_SUCCESS;
//
//	*assignable = true;
//	service_registration_pt registration = NULL;
//	status = serviceReference_getServiceRegistration(reference, &registration);
//	if (status == CELIX_SUCCESS) {
//		char *serviceName = properties_get(registration->properties, (char *) OBJECTCLASS);
//		if (!serviceReference_isAssignableTo(reference, requester, serviceName)) {
//			*assignable = false;
//		}
//	}
//
//	return status;
//}

long framework_getNextBundleId(framework_pt framework) {
	long id = framework->nextBundleId;
	framework->nextBundleId++;
	return id;
}

celix_status_t framework_markResolvedModules(framework_pt framework, linked_list_pt resolvedModuleWireMap) {
	if (resolvedModuleWireMap != NULL) {
		// hash_map_iterator_pt iterator = hashMapIterator_create(resolvedModuleWireMap);
		linked_list_iterator_pt iterator = linkedListIterator_create(resolvedModuleWireMap, linkedList_size(resolvedModuleWireMap));
		while (linkedListIterator_hasPrevious(iterator)) {
		    importer_wires_pt iw = linkedListIterator_previous(iterator);
			// hash_map_entry_pt entry = hashMapIterator_nextEntry(iterator);
			module_pt module = iw->importer;

//			bundle_pt bundle = module_getBundle(module);
//			bundle_archive_pt archive = NULL;
//			bundle_getArchive(bundle, &archive);
//			bundle_revision_pt revision = NULL;
//			bundleArchive_getCurrentRevision(archive, &revision);
//			char *root = NULL;
//			bundleRevision_getRoot(revision, &root);
//			manifest_pt manifest = NULL;
//			bundleRevision_getManifest(revision, &manifest);
//
//			char *private = manifest_getValue(manifest, OSGI_FRAMEWORK_PRIVATE_LIBRARY);
//			char *export = manifest_getValue(manifest, OSGI_FRAMEWORK_EXPORT_LIBRARY);
//
//			printf("Root %s\n", root);

			// for each library update the reference to the wires, if there are any

			linked_list_pt wires = iw->wires;

//			linked_list_iterator_pt wit = linkedListIterator_create(wires, 0);
//			while (linkedListIterator_hasNext(wit)) {
//			    wire_pt wire = linkedListIterator_next(wit);
//			    module_pt importer = NULL;
//			    requirement_pt requirement = NULL;
//			    module_pt exporter = NULL;
//                capability_pt capability = NULL;
//			    wire_getImporter(wire, &importer);
//			    wire_getRequirement(wire, &requirement);
//
//			    wire_getExporter(wire, &exporter);
//			    wire_getCapability(wire, &capability);
//
//			    char *importerName = NULL;
//			    module_getSymbolicName(importer, &importerName);
//
//			    char *exporterName = NULL;
//                module_getSymbolicName(exporter, &exporterName);
//
//                version_pt version = NULL;
//                char *name = NULL;
//                capability_getServiceName(capability, &name);
//                capability_getVersion(capability, &version);
//                char *versionString = NULL;
//                version_toString(version, framework->mp, &versionString);
//
//                printf("Module %s imports library %s:%s from %s\n", importerName, name, versionString, exporterName);
//			}

			module_setWires(module, wires);

			module_setResolved(module);
			resolver_moduleResolved(module);

			char *mname = NULL;
			module_getSymbolicName(module, &mname);
			framework_markBundleResolved(framework, module);
			linkedListIterator_remove(iterator);
			free(iw);
		}
		linkedListIterator_destroy(iterator);
		linkedList_destroy(resolvedModuleWireMap);
	}
	return CELIX_SUCCESS;
}

celix_status_t framework_markBundleResolved(framework_pt framework, module_pt module) {
    celix_status_t status = CELIX_SUCCESS;
	bundle_pt bundle = module_getBundle(module);
	bundle_state_e state;
	char *error = NULL;

	if (bundle != NULL) {
		framework_acquireBundleLock(framework, bundle, OSGI_FRAMEWORK_BUNDLE_INSTALLED|OSGI_FRAMEWORK_BUNDLE_RESOLVED|OSGI_FRAMEWORK_BUNDLE_ACTIVE);
		bundle_getState(bundle, &state);
		if (state != OSGI_FRAMEWORK_BUNDLE_INSTALLED) {
			printf("Trying to resolve a resolved bundle");
			status = CELIX_ILLEGAL_STATE;
		} else {
		    // Load libraries of this module
		    bool isSystemBundle = false;
		    bundle_isSystemBundle(bundle, &isSystemBundle);
		    if (!isSystemBundle) {
                status = CELIX_DO_IF(status, framework_loadBundleLibraries(framework, bundle));
		    }

		    status = CELIX_DO_IF(status, framework_setBundleStateAndNotify(framework, bundle, OSGI_FRAMEWORK_BUNDLE_RESOLVED));
			status = CELIX_DO_IF(status, fw_fireBundleEvent(framework, OSGI_FRAMEWORK_BUNDLE_EVENT_RESOLVED, bundle));
		}

		if (status != CELIX_SUCCESS) {
            module_pt module = NULL;
            char *symbolicName = NULL;
            long id = 0;
            module_getSymbolicName(module, &symbolicName);
            bundle_getBundleId(bundle, &id);
            if (error != NULL) {
                fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not start bundle: %s [%ld]; cause: %s", symbolicName, id, error);
            } else {
                fw_logCode(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, status, "Could not start bundle: %s [%ld]", symbolicName, id);
            }
        }

		framework_releaseBundleLock(framework, bundle);
	}

	return CELIX_SUCCESS;
}

array_list_pt framework_getBundles(framework_pt framework) {
	array_list_pt bundles = NULL;
	hash_map_iterator_pt iterator;
	arrayList_create(&bundles);

	celixThreadMutex_lock(&framework->installedBundleMapLock);

	iterator = hashMapIterator_create(framework->installedBundleMap);
	while (hashMapIterator_hasNext(iterator)) {
		bundle_pt bundle = (bundle_pt) hashMapIterator_nextValue(iterator);
		arrayList_add(bundles, bundle);
	}
	hashMapIterator_destroy(iterator);

	celixThreadMutex_unlock(&framework->installedBundleMapLock);

	return bundles;
}

bundle_pt framework_getBundle(framework_pt framework, char * location) {
	celixThreadMutex_lock(&framework->installedBundleMapLock);
	bundle_pt bundle = (bundle_pt) hashMap_get(framework->installedBundleMap, location);
	celixThreadMutex_unlock(&framework->installedBundleMapLock);
	return bundle;
}

bundle_pt framework_getBundleById(framework_pt framework, long id) {
	celixThreadMutex_lock(&framework->installedBundleMapLock);
	hash_map_iterator_pt iter = hashMapIterator_create(framework->installedBundleMap);
	bundle_pt bundle = NULL;
	while (hashMapIterator_hasNext(iter)) {
		bundle_pt b = (bundle_pt) hashMapIterator_nextValue(iter);
		bundle_archive_pt archive = NULL;
		long bid;
		bundle_getArchive(b, &archive);
		bundleArchive_getId(archive, &bid);
		if (bid == id) {
			bundle = b;
			break;
		}
	}
	hashMapIterator_destroy(iter);
	celixThreadMutex_unlock(&framework->installedBundleMapLock);

	return bundle;
}

celix_status_t framework_acquireInstallLock(framework_pt framework, char * location) {
    celixThreadMutex_lock(&framework->installRequestLock);

	while (hashMap_get(framework->installRequestMap, location) != NULL) {
	    celixThreadCondition_wait(&framework->condition, &framework->installRequestLock);
	}
	hashMap_put(framework->installRequestMap, location, location);

	celixThreadMutex_unlock(&framework->installRequestLock);

	return CELIX_SUCCESS;
}

celix_status_t framework_releaseInstallLock(framework_pt framework, char * location) {
    celixThreadMutex_lock(&framework->installRequestLock);

	hashMap_remove(framework->installRequestMap, location);
	celixThreadCondition_broadcast(&framework->condition);

	celixThreadMutex_unlock(&framework->installRequestLock);

	return CELIX_SUCCESS;
}

celix_status_t framework_setBundleStateAndNotify(framework_pt framework, bundle_pt bundle, int state) {
	int ret = CELIX_SUCCESS;

	int err = celixThreadMutex_lock(&framework->bundleLock);
	if (err != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Failed to lock");
		return CELIX_BUNDLE_EXCEPTION;
	}

	bundle_setState(bundle, state);
	err = celixThreadCondition_broadcast(&framework->condition);
	if (err != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Failed to broadcast");
		ret = CELIX_BUNDLE_EXCEPTION;
	}

	err = celixThreadMutex_unlock(&framework->bundleLock);
	if (err != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Failed to unlock");
		return CELIX_BUNDLE_EXCEPTION;
	}
	return ret;
}

celix_status_t framework_acquireBundleLock(framework_pt framework, bundle_pt bundle, int desiredStates) {
	celix_status_t status = CELIX_SUCCESS;

	bool locked;
	celix_thread_t lockingThread = celix_thread_default;

	int err = celixThreadMutex_lock(&framework->bundleLock);
	if (err != CELIX_SUCCESS) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Failed to lock");
		status = CELIX_BUNDLE_EXCEPTION;
	} else {
		bool lockable = false;
		bool isSelf = false;

		bundle_isLockable(bundle, &lockable);
		thread_equalsSelf(framework->globalLockThread, &isSelf);

		while (!lockable
				|| (( celixThread_initalized(framework->globalLockThread) == true)
				&& !isSelf)) {
			bundle_state_e state;
			bundle_getState(bundle, &state);
			if ((desiredStates & state) == 0) {
				status = CELIX_ILLEGAL_STATE;
				break;
			} else
				bundle_getLockingThread(bundle, &lockingThread);
				if (isSelf
					&& (celixThread_initalized(lockingThread) == true)
					&& arrayList_contains(framework->globalLockWaitersList, &lockingThread)) {
				framework->interrupted = true;
//				celixThreadCondition_signal_thread_np(&framework->condition, bundle_getLockingThread(bundle));
				celixThreadCondition_signal(&framework->condition);
			}

            celixThreadCondition_wait(&framework->condition, &framework->bundleLock);

			status = bundle_isLockable(bundle, &lockable);
			if (status != CELIX_SUCCESS) {
				break;
			}
		}

		if (status == CELIX_SUCCESS) {
			bundle_state_e state;
			bundle_getState(bundle, &state);
			if ((desiredStates & state) == 0) {
				status = CELIX_ILLEGAL_STATE;
			} else {
				if (bundle_lock(bundle, &locked)) {
					if (!locked) {
						status = CELIX_ILLEGAL_STATE;
					}
				}
			}
		}
		celixThreadMutex_unlock(&framework->bundleLock);
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to get bundle lock");

	return status;
}

bool framework_releaseBundleLock(framework_pt framework, bundle_pt bundle) {
    bool unlocked;
    celix_thread_t lockingThread = celix_thread_default;

    celixThreadMutex_lock(&framework->bundleLock);

    bundle_unlock(bundle, &unlocked);
	if (!unlocked) {
	    celixThreadMutex_unlock(&framework->bundleLock);
		return false;
	}
	bundle_getLockingThread(bundle, &lockingThread);
	if (celixThread_initalized(lockingThread) == false) {
	    celixThreadCondition_broadcast(&framework->condition);
	}

	celixThreadMutex_unlock(&framework->bundleLock);

	return true;
}

bool framework_acquireGlobalLock(framework_pt framework) {
    bool interrupted = false;
	bool isSelf = false;

	celixThreadMutex_lock(&framework->bundleLock);

	thread_equalsSelf(framework->globalLockThread, &isSelf);

	while (!interrupted
			&& (celixThread_initalized(framework->globalLockThread) == true)
			&& (!isSelf)) {
		celix_thread_t currentThread = celixThread_self();
		arrayList_add(framework->globalLockWaitersList, &currentThread);
		celixThreadCondition_broadcast(&framework->condition);

		celixThreadCondition_wait(&framework->condition, &framework->bundleLock);
		if (framework->interrupted) {
			interrupted = true;
			framework->interrupted = false;
		}

		arrayList_removeElement(framework->globalLockWaitersList, &currentThread);
	}

	if (!interrupted) {
		framework->globalLockCount++;
		framework->globalLockThread = celixThread_self();
	}

	celixThreadMutex_unlock(&framework->bundleLock);

	return !interrupted;
}

celix_status_t framework_releaseGlobalLock(framework_pt framework) {
	int status = CELIX_SUCCESS;
	if (celixThreadMutex_lock(&framework->bundleLock) != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error locking framework bundle lock");
		return CELIX_FRAMEWORK_EXCEPTION;
	}

	if (celixThread_equals(framework->globalLockThread, celixThread_self())) {
		framework->globalLockCount--;
		if (framework->globalLockCount == 0) {
			framework->globalLockThread = celix_thread_default;
 			if (celixThreadCondition_broadcast(&framework->condition) != 0) {
				fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Failed to broadcast global lock release.");
				status = CELIX_FRAMEWORK_EXCEPTION;
				// still need to unlock before returning
			}
		}
	} else {
		printf("The current thread does not own the global lock");
	}

	if (celixThreadMutex_unlock(&framework->bundleLock) != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error unlocking framework bundle lock");
		return CELIX_FRAMEWORK_EXCEPTION;
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to release global lock");

	return status;
}

celix_status_t framework_waitForStop(framework_pt framework) {
	if (celixThreadMutex_lock(&framework->mutex) != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error locking the framework, shutdown gate not set.");
		return CELIX_FRAMEWORK_EXCEPTION;
	}
	while (!framework->shutdown) {
	    celix_status_t status = celixThreadCondition_wait(&framework->shutdownGate, &framework->mutex);
		if (status != 0) {
			fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error waiting for shutdown gate.");
			return CELIX_FRAMEWORK_EXCEPTION;
		}
	}
	if (celixThreadMutex_unlock(&framework->mutex) != 0) {
		fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error unlocking the framework.");
		return CELIX_FRAMEWORK_EXCEPTION;
	}

	celixThread_join(framework->shutdownThread, NULL);

	fw_log(framework->logger, OSGI_FRAMEWORK_LOG_INFO, "FRAMEWORK: Successful shutdown");
	return CELIX_SUCCESS;
}

static void *framework_shutdown(void *framework) {
	framework_pt fw = (framework_pt) framework;
	int err;

	fw_log(fw->logger, OSGI_FRAMEWORK_LOG_INFO, "FRAMEWORK: Shutdown");
	celixThreadMutex_lock(&fw->installedBundleMapLock);

	hash_map_iterator_pt iter = hashMapIterator_create(fw->installedBundleMap);
	bundle_pt bundle = NULL;
	while ((bundle = hashMapIterator_nextValue(iter)) != NULL) {
        bundle_state_e state;
        bundle_getState(bundle, &state);
        if (state == OSGI_FRAMEWORK_BUNDLE_ACTIVE || state == OSGI_FRAMEWORK_BUNDLE_STARTING) {
            celixThreadMutex_unlock(&fw->installedBundleMapLock);
            fw_stopBundle(fw, bundle, 0);
            celixThreadMutex_lock(&fw->installedBundleMapLock);
            hashMapIterator_destroy(iter);
            iter = hashMapIterator_create(fw->installedBundleMap);
        }
	}
    hashMapIterator_destroy(iter);

    iter = hashMapIterator_create(fw->installedBundleMap);
	bundle = NULL;
	while ((bundle = hashMapIterator_nextValue(iter)) != NULL) {
		bundle_close(bundle);
	}
	hashMapIterator_destroy(iter);
	celixThreadMutex_unlock(&fw->installedBundleMapLock);

    err = celixThreadMutex_lock(&fw->mutex);
    if (err != 0) {
        fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error locking the framework, cannot exit clean.");
        celixThread_exit(NULL);
        return NULL;
    }

	if (celixThreadMutex_lock(&fw->dispatcherLock) != CELIX_SUCCESS) {
		fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error locking the dispatcherThread.");
	}
	else {
		fw->shutdown = true;

		if (celixThreadCondition_broadcast(&fw->dispatcher)) {
			fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error broadcasting .");
		}

		if (celixThreadMutex_unlock(&fw->dispatcherLock)) {
			fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR, "Error unlocking the dispatcherThread.");
		}

		celixThread_join(fw->dispatcherThread, NULL);
	}


	err = celixThreadCondition_broadcast(&fw->shutdownGate);
	if (err != 0) {
		fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error waking the shutdown gate, cannot exit clean.");
		err = celixThreadMutex_unlock(&fw->mutex);
		if (err != 0) {
			fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error unlocking the framework, cannot exit clean.");
		}

		celixThread_exit(NULL);
		return NULL;
	}
	err = celixThreadMutex_unlock(&fw->mutex);
	if (err != 0) {
//		fw_log(fw->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error unlocking the framework, cannot exit clean.");
	}

//	fw_log(fw->logger, OSGI_FRAMEWORK_LOG_INFO, "FRAMEWORK: Shutdown done\n");
	celixThread_exit((void *) CELIX_SUCCESS);

	return NULL;
}

celix_status_t framework_getFrameworkBundle(framework_pt framework, bundle_pt *bundle) {
	celix_status_t status = CELIX_SUCCESS;

	if (framework != NULL && *bundle == NULL) {
		*bundle = framework->bundle;
	} else {
		status = CELIX_ILLEGAL_ARGUMENT;
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to get framework bundle");

	return status;
}

celix_status_t fw_fireBundleEvent(framework_pt framework, bundle_event_type_e eventType, bundle_pt bundle) {
	celix_status_t status = CELIX_SUCCESS;

	if ((eventType != OSGI_FRAMEWORK_BUNDLE_EVENT_STARTING)
			&& (eventType != OSGI_FRAMEWORK_BUNDLE_EVENT_STOPPING)
			&& (eventType != OSGI_FRAMEWORK_BUNDLE_EVENT_LAZY_ACTIVATION)) {
		request_pt request = (request_pt) calloc(1, sizeof(*request));
		if (!request) {
			status = CELIX_ENOMEM;
        } else {
            bundle_archive_pt archive = NULL;
            module_pt module = NULL;

            request->eventType = eventType;
            request->filter = NULL;
            request->listeners = framework->bundleListeners;
            request->type = BUNDLE_EVENT_TYPE;
            request->error = NULL;
            request->bundleId = -1;

            status = bundle_getArchive(bundle, &archive);

            if (status == CELIX_SUCCESS) {
                long bundleId;

                status = bundleArchive_getId(archive, &bundleId);

                if (status == CELIX_SUCCESS) {
                    request->bundleId = bundleId;
                }
            }

            if (status == CELIX_SUCCESS) {
                status = bundle_getCurrentModule(bundle, &module);

                if (status == CELIX_SUCCESS) {
                    char *symbolicName = NULL;
                    status = module_getSymbolicName(module, &symbolicName);
                    if (status == CELIX_SUCCESS) {
                        request->bundleSymbolicName = strdup(symbolicName);
                    }
                }
            }

            if (celixThreadMutex_lock(&framework->dispatcherLock) != CELIX_SUCCESS) {
                status = CELIX_FRAMEWORK_EXCEPTION;
            } else {
                arrayList_add(framework->requests, request);
                if (celixThreadCondition_broadcast(&framework->dispatcher)) {
                    status = CELIX_FRAMEWORK_EXCEPTION;
                } else {
                    if (celixThreadMutex_unlock(&framework->dispatcherLock)) {
                        status = CELIX_FRAMEWORK_EXCEPTION;
                    }
                }
            }
        }
    }

    framework_logIfError(framework->logger, status, NULL, "Failed to fire bundle event");

	return status;
}

celix_status_t fw_fireFrameworkEvent(framework_pt framework, framework_event_type_e eventType, bundle_pt bundle, celix_status_t errorCode) {
	celix_status_t status = CELIX_SUCCESS;

	request_pt request = (request_pt) malloc(sizeof(*request));
	if (!request) {
		status = CELIX_ENOMEM;
	} else {
        bundle_archive_pt archive = NULL;
        module_pt module = NULL;

        request->eventType = eventType;
        request->filter = NULL;
        request->listeners = framework->frameworkListeners;
        request->type = FRAMEWORK_EVENT_TYPE;
        request->errorCode = errorCode;
        request->error = "";
        request->bundleId = -1;

        status = bundle_getArchive(bundle, &archive);

        if (status == CELIX_SUCCESS) {
            long bundleId;

            status = bundleArchive_getId(archive, &bundleId);

            if (status == CELIX_SUCCESS) {
                request->bundleId = bundleId;
            }
        }

        if (status == CELIX_SUCCESS) {
            status = bundle_getCurrentModule(bundle, &module);

            if (status == CELIX_SUCCESS) {
                char *symbolicName = NULL;
                status = module_getSymbolicName(module, &symbolicName);
                if (status == CELIX_SUCCESS) {
                    request->bundleSymbolicName = strdup(symbolicName);
                }
            }
        }

        if (errorCode != CELIX_SUCCESS) {
            char message[256];
            celix_strerror(errorCode, message, 256);
            request->error = message;
        }

        if (celixThreadMutex_lock(&framework->dispatcherLock) != CELIX_SUCCESS) {
            status = CELIX_FRAMEWORK_EXCEPTION;
        } else {
            arrayList_add(framework->requests, request);
            if (celixThreadCondition_broadcast(&framework->dispatcher)) {
                status = CELIX_FRAMEWORK_EXCEPTION;
            } else {
                if (celixThreadMutex_unlock(&framework->dispatcherLock)) {
                    status = CELIX_FRAMEWORK_EXCEPTION;
                }
            }
        }
    }

	framework_logIfError(framework->logger, status, NULL, "Failed to fire framework event");

	return status;
}

static void *fw_eventDispatcher(void *fw) {
	framework_pt framework = (framework_pt) fw;

	while (true) {
		int size;
		celix_status_t status;

		if (celixThreadMutex_lock(&framework->dispatcherLock) != 0) {
			fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error locking the dispatcher");
			celixThread_exit(NULL);
			return NULL;
		}

		size = arrayList_size(framework->requests);
		while (size == 0 && !framework->shutdown) {
			celixThreadCondition_wait(&framework->dispatcher, &framework->dispatcherLock);
			// Ignore status and just keep waiting
			size = arrayList_size(framework->requests);
		}

		if (size == 0 && framework->shutdown) {
		    celixThreadMutex_unlock(&framework->dispatcherLock);
			celixThread_exit(NULL);
			return NULL;
		}

		request_pt request = (request_pt) arrayList_remove(framework->requests, 0);

		if ((status = celixThreadMutex_unlock(&framework->dispatcherLock)) != 0) {
			fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Error unlocking the dispatcher.");
			celixThread_exit(NULL);
			return NULL;
		}

        if (celixThreadMutex_lock(&framework->bundleListenerLock) != CELIX_SUCCESS) {
            status = CELIX_FRAMEWORK_EXCEPTION;
        } else if (celixThreadMutex_lock(&framework->bundleLock) != CELIX_SUCCESS) {
            celixThreadMutex_unlock(&framework->bundleListenerLock);
            status = CELIX_FRAMEWORK_EXCEPTION;
        } else {
            int i;
            int size = arrayList_size(request->listeners);
            for (i = 0; i < size; i++) {
                if (request->type == BUNDLE_EVENT_TYPE) {
                    fw_bundle_listener_pt listener = (fw_bundle_listener_pt) arrayList_get(request->listeners, i);
                    bundle_event_pt event = (bundle_event_pt) calloc(1, sizeof(*event));
                    event->bundleId = request->bundleId;
                    event->bundleSymbolicName = strdup(request->bundleSymbolicName);
                    event->type = request->eventType;

                    fw_invokeBundleListener(framework, listener->listener, event, listener->bundle);

                    free(event->bundleSymbolicName);
                    free(event);
                } else if (request->type == FRAMEWORK_EVENT_TYPE) {
                    fw_framework_listener_pt listener = (fw_framework_listener_pt) arrayList_get(request->listeners, i);
                    framework_event_pt event = (framework_event_pt) calloc(1, sizeof(*event));
                    event->bundleId = request->bundleId;
                    event->bundleSymbolicName = strdup(request->bundleSymbolicName);
                    event->type = request->eventType;
                    event->error = request->error;
                    event->errorCode = request->errorCode;

                    fw_invokeFrameworkListener(framework, listener->listener, event, listener->bundle);

                    free(event);
                }
            }

            if (celixThreadMutex_unlock(&framework->bundleLock)) {
                status = CELIX_FRAMEWORK_EXCEPTION;
            }

            if (celixThreadMutex_unlock(&framework->bundleListenerLock)) {
                status = CELIX_FRAMEWORK_EXCEPTION;
            }

            free(request->bundleSymbolicName);
            free(request);
        }

    }

	celixThread_exit(NULL);

	return NULL;

}

celix_status_t fw_invokeBundleListener(framework_pt framework, bundle_listener_pt listener, bundle_event_pt event, bundle_pt bundle) {
	// We only support async bundle listeners for now
	bundle_state_e state;
	celix_status_t ret = bundle_getState(bundle, &state);
	if (state == OSGI_FRAMEWORK_BUNDLE_STARTING || state == OSGI_FRAMEWORK_BUNDLE_ACTIVE) {

		listener->bundleChanged(listener, event);
	}

	return ret;
}

celix_status_t fw_invokeFrameworkListener(framework_pt framework, framework_listener_pt listener, framework_event_pt event, bundle_pt bundle) {
	bundle_state_e state;
	celix_status_t ret = bundle_getState(bundle, &state);
	if (state == OSGI_FRAMEWORK_BUNDLE_STARTING || state == OSGI_FRAMEWORK_BUNDLE_ACTIVE) {
		listener->frameworkEvent(listener, event);
	}

	return ret;
}

static celix_status_t frameworkActivator_start(void * userData, bundle_context_pt context) {
	// nothing to do
	return CELIX_SUCCESS;
}

static celix_status_t frameworkActivator_stop(void * userData, bundle_context_pt context) {
    celix_status_t status = CELIX_SUCCESS;
	framework_pt framework;

	if (bundleContext_getFramework(context, &framework) == CELIX_SUCCESS) {

	    fw_log(framework->logger, OSGI_FRAMEWORK_LOG_INFO, "FRAMEWORK: Start shutdownthread");
	    if (celixThread_create(&framework->shutdownThread, NULL, &framework_shutdown, framework) != CELIX_SUCCESS) {
            fw_log(framework->logger, OSGI_FRAMEWORK_LOG_ERROR,  "Could not create shutdown thread, normal exit not possible.");
	        status = CELIX_FRAMEWORK_EXCEPTION;
	    }
	} else {
		status = CELIX_FRAMEWORK_EXCEPTION;
	}

	framework_logIfError(framework->logger, status, NULL, "Failed to stop framework activator");

	return status;
}

static celix_status_t frameworkActivator_destroy(void * userData, bundle_context_pt context) {
	return CELIX_SUCCESS;
}


static celix_status_t framework_loadBundleLibraries(framework_pt framework, bundle_pt bundle) {
    celix_status_t status = CELIX_SUCCESS;

    handle_t handle = NULL;
    bundle_archive_pt archive = NULL;
    bundle_revision_pt revision = NULL;
    manifest_pt manifest = NULL;

    status = CELIX_DO_IF(status, bundle_getArchive(bundle, &archive));
    status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
    status = CELIX_DO_IF(status, bundleRevision_getManifest(revision, &manifest));
    if (status == CELIX_SUCCESS) {
        char *privateLibraries = NULL;
        char *exportLibraries = NULL;
        char *activator = NULL;

        privateLibraries = manifest_getValue(manifest, OSGI_FRAMEWORK_PRIVATE_LIBRARY);
        exportLibraries = manifest_getValue(manifest, OSGI_FRAMEWORK_EXPORT_LIBRARY);
        activator = manifest_getValue(manifest, OSGI_FRAMEWORK_BUNDLE_ACTIVATOR);

        if (exportLibraries != NULL) {
            status = CELIX_DO_IF(status, framework_loadLibraries(framework, exportLibraries, activator, archive, &handle));
        }

        if (privateLibraries != NULL) {
            status = CELIX_DO_IF(status,
                                 framework_loadLibraries(framework, privateLibraries, activator, archive, &handle));
        }

        if (status == CELIX_SUCCESS) {
            bundle_setHandle(bundle, handle);
        }
    }

    framework_logIfError(framework->logger, status, NULL, "Could not load all bundle libraries");

    return status;
}

// TODO Store all handles for unloading!
static celix_status_t framework_loadLibraries(framework_pt framework, char *libraries, char *activator, bundle_archive_pt archive, void **activatorHandle) {
    celix_status_t status = CELIX_SUCCESS;

    char *last;
    char *token = strtok_r(libraries, ",", &last);
    while (token != NULL) {
        void *handle = NULL;
        char lib[128];
        lib[127] = '\0';

        char *path;
        char *pathToken = strtok_r(token, ";", &path);
        strncpy(lib, pathToken, 127);
        pathToken = strtok_r(NULL, ";", &path);

        while (pathToken != NULL) {

            /*Disable version should be part of the lib name
            if (strncmp(pathToken, "version", 7) == 0) {
                char *ver = strdup(pathToken);
                char version[strlen(ver) - 9];
                strncpy(version, ver+9, strlen(ver) - 10);
                version[strlen(ver) - 10] = '\0';

                strcat(lib, "-");
                strcat(lib, version);
            }*/
            pathToken = strtok_r(NULL, ";", &path);
        }

        char *trimmedLib = utils_stringTrim(lib);
        status = framework_loadLibrary(framework, trimmedLib, archive, &handle);

        if (status == CELIX_SUCCESS) {
            if (activator != NULL) {
                if (strcmp(trimmedLib, activator) == 0) {
                    *activatorHandle = handle;
                }
            }
        }

        token = strtok_r(NULL, ",", &last);
    }

    framework_logIfError(framework->logger, status, NULL, "Could not load all libraries");

    return status;
}

static celix_status_t framework_loadLibrary(framework_pt framework, char *library, bundle_archive_pt archive, void **handle) {
    celix_status_t status = CELIX_SUCCESS;
    char *error = NULL;

    #ifdef __linux__
        char * library_prefix = "lib";
        char * library_extension = ".so";
    #elif __APPLE__
        char * library_prefix = "lib";
        char * library_extension = ".dylib";
    #elif WIN32
        char * library_prefix = "";
        char * library_extension = ".dll";
    #endif

    char libraryPath[256];
    long refreshCount = 0;
    char *archiveRoot = NULL;
    long revisionNumber = 0;

    status = CELIX_DO_IF(status, bundleArchive_getRefreshCount(archive, &refreshCount));
    status = CELIX_DO_IF(status, bundleArchive_getArchiveRoot(archive, &archiveRoot));
    status = CELIX_DO_IF(status, bundleArchive_getCurrentRevisionNumber(archive, &revisionNumber));

    memset(libraryPath, 0, 256);
    int written = 0;
    if (strncmp("lib", library, 3) == 0) {
        written = snprintf(libraryPath, 256, "%s/version%ld.%ld/%s", archiveRoot, refreshCount, revisionNumber, library);
    } else {
        written = snprintf(libraryPath, 256, "%s/version%ld.%ld/%s%s%s", archiveRoot, refreshCount, revisionNumber, library_prefix, library, library_extension);
    }

    if (written >= 256) {
    	error = "library path is too long";
    	status = CELIX_FRAMEWORK_EXCEPTION;
    } else {
		*handle = fw_openLibrary(libraryPath);
		if (*handle == NULL) {
			error = fw_getLastError();
			// #TODO this is wrong
			status =  CELIX_BUNDLE_EXCEPTION;
		} else {
			bundle_revision_pt revision = NULL;
			array_list_pt handles = NULL;

			status = CELIX_DO_IF(status, bundleArchive_getCurrentRevision(archive, &revision));
			status = CELIX_DO_IF(status, bundleRevision_getHandles(revision, &handles));

			arrayList_add(handles, *handle);
		}
    }

    framework_logIfError(framework->logger, status, error, "Could not load library: %s", libraryPath);

    return status;
}
