# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

install (FILES 
        public/include/remote_endpoint_impl.h
        public/include/remote_endpoint.h
        public/include/remote_proxy.h
        public/include/remote_service_admin.h
        public/include/export_registration.h
        public/include/import_registration.h
        public/include/endpoint_description.h
        public/include/endpoint_listener.h
        public/include/remote_constants.h
    DESTINATION 
        include/celix/remote_service_admin 
    COMPONENT 
        remote_service_admin
)
install (FILES
        private/src/remote_proxy_factory_impl.c
    DESTINATION
        share/celix/remote_service_admin
    COMPONENT
        remote_service_admin
)
install (FILES 
        public/include/endpoint_listener.h
    DESTINATION 
        include/celix/endpoint_listener 
    COMPONENT 
        remote_service_admin
)
