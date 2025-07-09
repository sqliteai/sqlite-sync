## RLS Policies


### Users

#### SELECT

```sql
auth_userid() = user_id OR json_extract(auth_json(), '$.name') = 'coach'
```

#### INSERT

```sql
auth_userid() = NEW.id
```

#### UPDATE

_No policy_

#### DELETE

_No policy_

---

### Activities 

#### SELECT

```sql
auth_userid() = user_id OR json_extract(auth_json(), '$.name') = 'coach'
```

#### INSERT

```sql
auth_userid() = NEW.user_id
```

#### UPDATE

_No policy_

#### DELETE

```sql
auth_userid() = OLD.user_id OR json_extract(auth_json(), '$.name') = 'coach'
```

---

### Workouts

#### SELECT

```sql
auth_userid() = user_id OR json_extract(auth_json(), '$.name') = 'coach'
```

#### INSERT

```sql
json_extract(auth_json(), '$.name') = 'coach'
```

#### UPDATE

```sql
OLD.user_id = auth_userid() OR json_extract(auth_json(), '$.name') = 'coach'
```

#### DELETE

```sql
json_extract(auth_json(), '$.name') = 'coach'
```
