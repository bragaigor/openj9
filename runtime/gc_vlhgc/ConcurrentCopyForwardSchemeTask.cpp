/*******************************************************************************
 * Copyright (c) 1991, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "omrcfg.h"

#if defined(OMR_GC_VLHGC_CONCURRENT_COPY_FORWARD)

#include "j9port.h"
#include "modronopt.h"
#include "omrcomp.h"

#include "ConcurrentCopyForwardSchemeTask.hpp"

void
MM_ConcurrentCopyForwardSchemeTask::run(MM_EnvironmentBase *envBase)
{
	MM_EnvironmentVLHGC *env = MM_EnvironmentVLHGC::getEnvironment(envBase);

	switch (_action) {
	case COPY_FORWARD_ALL:
		_copyForwardScheme->workThreadProcessRoots(env);
		_copyForwardScheme->workThreadScan(env);
		_copyForwardScheme->workThreadComplete(env);
		break;
	case COPY_FORWARD_ROOTS:
		_copyForwardScheme->workThreadProcessRoots(env);
		break;
	case COPY_FORWARD_SCAN:
		_copyForwardScheme->workThreadScan(env);
		break;
	case COPY_FORWARD_COMPLETE:
		_copyForwardScheme->workThreadComplete(env);
		break;
	default:
		Assert_MM_unreachable();
	}
}
#endif /* defined(OMR_GC_VLHGC_CONCURRENT_COPY_FORWARD) */
