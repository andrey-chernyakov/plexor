# Plexor

Plexor is remote function call PL language.

## Installation

    $ make
    $ make install

## Quick examples

```
create extension plexor;

create server my_cluster
  foreign data wrapper plexor
  options (isolation_level 'read committed', 
           node_0 'dbname=my_cluster_0 host=192.168.0.10',
           node_1 'dbname=my_cluster_1 host=192.168.0.11');

-- isolation_level { auto commit | read committed | read committed read only }
-- auto commit is analog of autonomous transaction

create user mapping 
  for public server my_cluster 
  options (user 'postgres'); 
```

Remote call of function with the same signature on node 0
```
create or replace function get_name(aperson_id integer)
returns text 
as $$
  cluster my_cluster;
  run on 0;
$$ language plexor;
```

Remote call of function with different signature on specified node
```
create or replace function get_name(anode integer, aperson_id integer)
returns text
as $$
  cluster my_cluster;
  run get_person_name(aperson_id) on anode;
$$ language plexor;
```

Remote call of function on node wich acquired by taking get_node(aperson_id)
```
create or replace function get_name(aperson_id integer)
returns text
as $$
  cluster my_cluster;
  run get_person_name(aperson_id) on get_node(aperson_id);
$$ language plexor;
```

