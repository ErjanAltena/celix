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
#{{
#import fnmatch
#import os
#import yaml
#project = None 
#with open(projectFile) as input :
#	project = yaml.load(input)
#}}
#{{end}}

cmake_minimum_required(VERSION 2.8)

#{{
#	cog.outl("project(%s C)" % project['name'])
#}}
project(myproject C) #do not edit, generated code
#{{end}}

#{{ 
#       cog.outl("set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} \"%s/share/celix/cmake/modules\")" % project['celix_install_dir'])
#       cog.outl("link_directories(\"%s/lib\")" % project['celix_install_dir'])
#}}
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "/usr/local/share/celix/cmake/modules") #do not edit, generated code
#{{end}}

find_package(CELIX REQUIRED)
include_directories(${CELIX_INCLUDE_DIRS})


#{{
#for root, dirs, filenames in os.walk('.'):
#	for foundFile in fnmatch.filter(filenames, 'bundle.yaml'):
#		cog.outl("add_subdirectory(%s)" % root)
#}}
#
#{{end}}

