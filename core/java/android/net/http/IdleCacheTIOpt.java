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

import java.util.LinkedList;

import android.os.SystemClock;

/**
 * {@hide}
 */
class IdleCacheTIOpt extends AbstractIdleCache{

    class Entry {
        HttpHost mHost;
        Connection mConnection;
        long mTimeout;

        Entry(HttpHost host, Connection connection, long timeout){
            mHost = host;
            mConnection = connection;
            mTimeout = timeout;
        }
    };

    private LinkedList<Entry> lstEntry = new LinkedList<Entry>();

    /*It is recommened to set it twice the CONNECTION_COUNT defined in
    RequestQueue.java. This way you can effectively use the cache (more hits and less misses).
    Since its changed to linked list, this is the upper bound on the link list size.
    */
    private final static int IDLE_CACHE_MAX = 100;

    /* Allow three consecutive empty queue checks before shutdown */
    private final static int EMPTY_CHECK_MAX = 2;

    /* six second timeout for connections */
    private final static int TIMEOUT = 6 * 1000;
    /*Changed from 2 sec, since the connection timeout is 6 sec. No point in waking
    up IdleReaper thread so often. Only issue is for EMPTY_CHECK_MAX which may be delayed
    from 5*2(=10sec) to 5*5(=25 sec). So reducing EMPTY_CHECK_MAX to 2*/
    private final static int CHECK_INTERVAL = 5 * 1000;

    private int mCount = 0;

    private IdleReaper mThread = null;

    /* stats */
    private int mCached = 0;
    private int mReused = 0;

    IdleCacheTIOpt() {
    }

    /**
     * Caches connection, if there is room.
     * @return true if connection cached
     */
    synchronized boolean cacheConnection(
            HttpHost host, Connection connection) {

        boolean ret = false;

        if (HttpLog.LOGV) {
            HttpLog.v("IdleCacheTIOpt size " + mCount + " host "  + host + "list size" + lstEntry.size());
        }

        if (mCount < IDLE_CACHE_MAX) {
            long time = SystemClock.uptimeMillis();
            lstEntry.addLast(new Entry(host, connection, time+TIMEOUT));

            mCount++;

            if (HttpLog.LOGV){
                mCached++;
                HttpLog.v("Cached Cnxn Increment" + mCount + "list size" + lstEntry.size());
            }

            ret = true;
            if (mThread == null) {
                mThread = new IdleReaper();
                mThread.start();
            }
        }
        return ret;
    }

    synchronized Connection getConnection(HttpHost host) {
        Connection ret = null;
        /**
        * Reuse the cached connection only after establishing
        *more http connection with server.
        */
        if (mCount > 0) {
            for(int i=0; i< lstEntry.size(); i++){
                Entry entry = lstEntry.get(i);
                HttpHost eHost = entry.mHost;
                if (eHost.equals(host)) {
                    ret = entry.mConnection;
                    lstEntry.remove(i);
                    mCount--;
                    if (HttpLog.LOGV){
                        mReused++;
                        HttpLog.v("Cached Cnxn Decrement" + mCount + "list size" + lstEntry.size());
                    }
                    break;
               }
            }
        }
        return ret;
    }

    synchronized void clear() {
        for (int i = 0; mCount > 0 && i < lstEntry.size(); i++) {
            Entry entry = lstEntry.get(i);
            if (entry.mHost != null) {
                entry.mConnection.closeConnection();
                lstEntry.remove(i);
                mCount--;
                if (HttpLog.LOGV) HttpLog.v("ClearCached Cnxn Decrement" + mCount + "list size" + lstEntry.size());
            }
        }
    }

    private synchronized void clearIdle() {
        if (mCount > 0) {
            long time = SystemClock.uptimeMillis();
            for (int i = 0; i < lstEntry.size(); i++) {
                Entry entry = lstEntry.get(i);
                if (time > entry.mTimeout) {
                    entry.mConnection.closeConnection();
                    lstEntry.remove(i);
                    mCount--;
                    if (HttpLog.LOGV) HttpLog.v("ClearIdleCached Cnxn Decrement" + mCount + "list size" + lstEntry.size());
                }
            }
        }
    }

    private class IdleReaper extends Thread {

        public void run() {
            int check = 0;

            setName("IdleReaper");
            android.os.Process.setThreadPriority(
                    android.os.Process.THREAD_PRIORITY_BACKGROUND);
            synchronized (IdleCacheTIOpt.this) {
                while (check < EMPTY_CHECK_MAX) {
                    try {
                        IdleCacheTIOpt.this.wait(CHECK_INTERVAL);
                    } catch (InterruptedException ex) {
                    }
                    if (mCount == 0) {
                        check++;
                    } else {
                        check = 0;
                        clearIdle();
                    }
                }
                mThread = null;
            }
            if (HttpLog.LOGV) {
                HttpLog.v("IdleCacheTIOpt IdleReaper shutdown: cached " + mCached +
                          " reused " + mReused);
                mCached = 0;
                mReused = 0;
            }
        }
    }
}
