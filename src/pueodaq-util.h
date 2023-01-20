#ifndef PUEO_DAQ_UTIL_H
#define PUEO_DAQ_UTIL_H

/*
*
* This file is part of pueodaq, 
*
* Various utility functions are defined here
*  
* Contributing Authors:  cozzyd@kicp.uchicago.edu
*
* Copyright (C) 2022-  PUEO Collaboration
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
* along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#include <stddef.h> 


// returns non-zero if the memory pointed at to by mem is all zeros 
int memallzero(const void * mem, size_t len);  





#endif
