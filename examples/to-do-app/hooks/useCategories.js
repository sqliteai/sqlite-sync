import { useState, useEffect } from 'react'
import { Platform } from 'react-native';
import { db } from "../db/dbConnection";
import { MANAGED_DATABASE_ID, API_TOKEN } from "@env";
import { getDylibPath } from "@op-engineering/op-sqlite";
import { randomUUID } from 'expo-crypto';
import { useSyncContext } from '../components/SyncContext';

const useCategories = () => {
  const [moreCategories, setMoreCategories] = useState(['Work', 'Personal'])
  const { registerRefreshCallback, setSync } = useSyncContext()

  const getCategories = async () => {
    try {
      const tags = await db.execute('SELECT * FROM tags')
      const allCategories = tags.rows.map(tag => tag.name)
      setMoreCategories(allCategories)
    } catch (error) {
      console.error('Error getting tags/categories', error)
    }
  }

  const addCategory = async newCategory => {
    try {
      await db.execute('INSERT INTO tags (uuid, name) VALUES (?, ?) RETURNING *', [randomUUID(), newCategory])
      db.execute('SELECT cloudsync_network_send_changes();')
      setMoreCategories(prevCategories => [...prevCategories, newCategory])
    } catch (error) {
      console.error('Error adding category', error)
    }
  }

  const removeCategory = async categoryName => {
    try {
      await db.execute('DELETE FROM tags WHERE name = ?', [categoryName])
      db.execute('SELECT cloudsync_network_send_changes();')
      setMoreCategories(prevCategories => prevCategories.filter(cat => cat !== categoryName))
    } catch (error) {
      console.error('Error removing category', error)
    }
  }

  const initializeTables = async () => {
    let extensionPath;

    console.log('Loading extension...');
    try {
      if (Platform.OS == "ios") {
        extensionPath = getDylibPath("ai.sqlite.cloudsync", "CloudSync");
      } else {
        extensionPath = 'cloudsync';
      }

      db.loadExtension(extensionPath);

      const version = await db.execute('SELECT cloudsync_version();');
      console.log(`Cloudsync extension loaded successfully, version: ${version.rows[0]['cloudsync_version()']}`);
    } catch (error) {
      console.error('Error loading cloudsync extension:', error);
    }

    console.log('Initializing tables and cloudsync...');
    try {
      await db.execute('CREATE TABLE IF NOT EXISTS tasks (uuid TEXT NOT NULL PRIMARY KEY, title TEXT, isCompleted INT NOT NULL DEFAULT 0);')
      await db.execute('CREATE TABLE IF NOT EXISTS tags (uuid TEXT NOT NULL PRIMARY KEY, name TEXT, UNIQUE(name));')
      await db.execute('CREATE TABLE IF NOT EXISTS tasks_tags (uuid TEXT NOT NULL PRIMARY KEY, task_uuid TEXT, tag_uuid TEXT, FOREIGN KEY (task_uuid) REFERENCES tasks(uuid), FOREIGN KEY (tag_uuid) REFERENCES tags(uuid));')

      await db.execute(`SELECT cloudsync_init('tasks');`);
      await db.execute(`SELECT cloudsync_init('tags');`);
      await db.execute(`SELECT cloudsync_init('tasks_tags');`);
      
      await db.execute('INSERT OR IGNORE INTO tags (uuid, name) VALUES (?, ?)', ['work', 'Work'])
      await db.execute('INSERT OR IGNORE INTO tags (uuid, name) VALUES (?, ?)', ['personal', 'Personal'])

      if (MANAGED_DATABASE_ID && API_TOKEN) {
        await db.execute(`SELECT cloudsync_network_init('${MANAGED_DATABASE_ID}');`);
        await db.execute(`SELECT cloudsync_network_set_token('${API_TOKEN}');`)
      } else {
        throw new Error('No valid MANAGED_DATABASE_ID or API_TOKEN provided, cloudsync_network_init will not be called');
      }

      db.execute('SELECT cloudsync_network_sync(100, 10);')
      getCategories()
      setSync(true)
    } catch (error) {
      console.error('Error creating tables', error)
    }
  }

  useEffect(() => {
    initializeTables()
  }, [])

  useEffect(() => {
    return registerRefreshCallback(() => {
      getCategories();
    });
  }, [registerRefreshCallback])

  return {
    moreCategories,
    addCategory,
    removeCategory,
    getCategories
  }
}

export default useCategories
