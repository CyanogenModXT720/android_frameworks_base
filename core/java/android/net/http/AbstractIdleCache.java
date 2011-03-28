/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Hangs onto idle live connections for a little while
 */

package android.net.http;

import org.apache.http.HttpHost;

/**
 * {@hide}
 */
public abstract class AbstractIdleCache {

/**
 * Caches connection, if there is room.
 * @return true if connection cached
 */
abstract boolean cacheConnection(
    HttpHost host, Connection connection);
    abstract Connection getConnection(HttpHost host);
    abstract void clear();
}

