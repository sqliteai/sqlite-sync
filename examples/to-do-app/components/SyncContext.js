import { createContext, useContext, useEffect, useState, useRef } from 'react';
import { InteractionManager, AppState } from 'react-native';
import { db } from '../db/dbConnection';

const SyncContext = createContext();

export const SyncProvider = ({ children }) => {
  const refreshCallbacks = useRef(new Set());
  const [isSyncEnabled, setIsSyncEnabled] = useState(false);
  const isCheckingRef = useRef(false);
  const lastCheckTimeRef = useRef(0);
  const appStateRef = useRef(AppState.currentState);
  const timeoutIdRef = useRef(null);

  const registerRefreshCallback = (callback) => {
    refreshCallbacks.current.add(callback);
    return () => refreshCallbacks.current.delete(callback);
  };

  const triggerRefresh = () => {
    refreshCallbacks.current.forEach(callback => callback());
  };

  const setSync = (enabled) => {
    setIsSyncEnabled(enabled);
  };

  useEffect(() => {
    if (!isSyncEnabled) return;

    const checkForChanges = () => {
      // Skip if already checking or app is in background
      if (isCheckingRef.current || appStateRef.current !== 'active') {
        timeoutIdRef.current = setTimeout(checkForChanges, 1000);
        return;
      }

      // Throttle checks - don't check more than once every 2 seconds
      const now = Date.now();
      if (now - lastCheckTimeRef.current < 2000) {
        timeoutIdRef.current = setTimeout(checkForChanges, 2000 - (now - lastCheckTimeRef.current));
        return;
      }

      // Use InteractionManager to wait for UI interactions to complete
      InteractionManager.runAfterInteractions(async () => {
        if (isCheckingRef.current) return;
        
        isCheckingRef.current = true;
        lastCheckTimeRef.current = Date.now();
        
        try {
          // Use a timeout for the database query to prevent hanging
          const queryPromise = db.execute('SELECT cloudsync_network_check_changes();');
          const timeoutPromise = new Promise((_, reject) => 
            setTimeout(() => reject(new Error('Query timeout')), 5000)
          );
          
          const result = await Promise.race([queryPromise, timeoutPromise]);
          
          const raw = result.rows?.[0]?.['cloudsync_network_check_changes()'];
          if (raw) {
            const { receive } = JSON.parse(raw);
            if (receive.rows > 0) {
              console.log(`${receive.rows} changes detected in [${receive.tables}], triggering refresh`);
              // Defer refresh to next tick to avoid blocking current interaction
              setTimeout(() => triggerRefresh(), 0);
            }
          }
        } catch (error) {
          console.error('Error checking for changes:', error);
        } finally {
          isCheckingRef.current = false;
          
          // Schedule next check with adaptive interval
          const nextInterval = appStateRef.current === 'active' ? 2500 : 10000;
          timeoutIdRef.current = setTimeout(checkForChanges, nextInterval);
        }
      });
    };

    // Handle app state changes
    const handleAppStateChange = (nextAppState) => {
      appStateRef.current = nextAppState;
      
      // If app becomes active and sync is enabled, check immediately
      if (nextAppState === 'active' && isSyncEnabled && !isCheckingRef.current) {
        if (timeoutIdRef.current) {
          clearTimeout(timeoutIdRef.current);
        }
        timeoutIdRef.current = setTimeout(checkForChanges, 100);
      }
    };

    const subscription = AppState.addEventListener('change', handleAppStateChange);

    // Start checking after a small delay to let UI settle
    timeoutIdRef.current = setTimeout(checkForChanges, 1000);

    return () => {
      if (timeoutIdRef.current) {
        clearTimeout(timeoutIdRef.current);
        timeoutIdRef.current = null;
      }
      isCheckingRef.current = false;
      subscription?.remove();
    };
  }, [isSyncEnabled]);

  return (
    <SyncContext.Provider value={{ registerRefreshCallback, setSync }}>
      {children}
    </SyncContext.Provider>
  );
};

export const useSyncContext = () => {
  const context = useContext(SyncContext);
  if (!context) {
    throw new Error('useSyncContext must be used within a SyncProvider');
  }
  return context;
};