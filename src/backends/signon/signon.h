/*
 * Copyright (C) 2013 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */
#ifndef INCL_SYNC_EVOLUTION_SIGNON_AUTH_PROVIDER
# define INCL_SYNC_EVOLUTION_SIGNON_AUTH_PROVIDER

#include <syncevo/util.h>

#include <memory>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#if defined(USE_ACCOUNTS) && defined(USE_GSSO) || defined(STATIC_GSSO)
# define SE_SIGNON_PROVIDER_ID "gsso"
#elif defined(USE_ACCOUNTS) && defined(USE_UOA) || defined(STATIC_UOA)
# define SE_SIGNON_PROVIDER_ID "uoa"
#elif defined(USE_SIGNON) || defined(STATIC_SIGNON)
# define SE_SIGNON_PROVIDER_ID "signon"
#endif

class AuthProvider;
std::shared_ptr<AuthProvider> createSignonAuthProvider(const InitStateString &username,
                                                         const InitStateString &password);

SE_END_CXX
#endif
