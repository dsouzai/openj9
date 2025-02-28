/*[INCLUDE-IF JAVA_SPEC_VERSION >= 8]*/
/*
 * Copyright IBM Corp. and others 2016
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
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 */
package com.ibm.gpu;

import java.security.BasicPermission;
import java.security.Permission;

/**
 * This class defines GPU permissions as described in the following table.
 *
 * <table border=1 style="padding:5px">
 * <caption>GPU Permissions</caption>
 * <tr>
 * <th style="text-align:left">Permission Name</th>
 * <th style="text-align:left">Allowed Action</th>
 * </tr>
 * <tr>
 * <td>access</td>
 * <td>Accessing the instance of CUDAManager.
 *     See {@link CUDAManager#instance()}.</td>
 * </tr>
 * </table>
 *
/*[IF JAVA_SPEC_VERSION >= 24]
 * @deprecated Checking permissions is not supported.
/*[ENDIF] JAVA_SPEC_VERSION >= 24
 */
/*[IF JAVA_SPEC_VERSION >= 24]*/
@Deprecated(since = "24", forRemoval = true)
/*[ENDIF] JAVA_SPEC_VERSION >= 24 */
public final class GPUPermission extends BasicPermission {

	static final Permission Access = new GPUPermission("access"); //$NON-NLS-1$

	private static final long serialVersionUID = -2838763669737231298L;

	/**
	 * Create a representation of the named permissions.
	 *
	 * @param name
	 *          name of the permission
	 */
	public GPUPermission(String name) {
		super(name);
	}

	/**
	 * Create a representation of the named permissions.
	 *
	 * @param name
	 *          name of the permission
	 * @param actions
	 *          not used, must be null or an empty string
	 * @throws IllegalArgumentException
	 *          if actions is not null or an empty string
	 */
	public GPUPermission(String name, String actions) {
		super(name, actions);

		// ensure that actions is null or an empty string
		if (!((null == actions) || actions.isEmpty())) {
			throw new IllegalArgumentException();
		}
	}

}
