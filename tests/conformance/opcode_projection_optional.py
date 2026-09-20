"""Optional-provider semantic projection.

Native records are read from actual managers, result stores and raw mappings.
Handle pool/generation policies may differ: a role table keeps every original
identity and requires bijection. Unrecognized provider fields remain unmapped.
Allocator journals and scheduler/resource state are semantic observations.
Missing native fields and unknown source state prevent a complete comparison.
"""
try:
    from .opcode_projection_common import identity, IdentityBijection
except ImportError:
    from opcode_projection_common import identity, IdentityBijection


def val(v):
    if v is None:
        return None
    if isinstance(v, list):
        return [val(x) for x in v]
    if not isinstance(v, dict):
        return v
    if set(v) == {'u64'}:
        return int(v['u64'])
    if set(v) == {'bool'}:
        return v['bool']
    if set(v) == {'unit'}:
        return None
    if set(v) == {'array'}:
        return [val(x) for x in v['array']]
    if set(v) == {'bytes'}:
        return {'bytes': [int(x) for x in v['bytes']]}
    if set(v) == {'handle'} or v.get('kind') == 'handle':
        return {'kind': 'handle', 'identity': identity(v)}
    kind = v.get('kind')
    if kind == 'bits':
        return int(v['value'])
    if kind == 'bool':
        return v['value']
    if kind == 'unit':
        return None
    if kind == 'bytes':
        return {'bytes': [int(x) for x in v['data']]}
    if kind in ('record', 'vec'):
        return [val(x) for x in v.get('fields', v.get('values', []))]
    if kind == 'variant':
        return {'tag': v['tag'], 'fields': [val(x) for x in v['fields']]}
    raise ValueError('unknown owning value form: '+repr(v))


def objects(outcome, tag, alive=True):
    return [o for o in outcome['world']['objects'] if o['tag'] == tag and (not alive or o['alive'])]


def id_key(v):
    return tuple(sorted(identity(v).items()))


def role_table(model, execute, native, input_value=None):
    named = {}
    for stage in ('initialIdentities', 'identities'):
        for item in native.get(stage, []):
            if not isinstance(item, dict) or set(item) != {'name', 'identity'}:
                raise ValueError('optional native identity table layout')
            if item['name'] in named and id_key(named[item['name']]) != id_key(item['identity']):
                raise ValueError('seeded identity changed generation/owner without a new lifetime')
            named[item['name']] = item['identity']
    sides = {}
    for side, outcome in [('model', model), ('exec', execute)]:
        entries = {}
        for tag, prefix in [('managed.lease', 'lease'), ('managed.access', 'access')]:
            for item in objects(outcome, tag, False):
                role = prefix+'.'+str(item['identity']['slot'])
                entries[role] = item['identity']
                if prefix == 'access' and item['alive']:
                    fields = val(item['value'])
                    entries['result.'+str(item['identity']['slot'])] = fields[6]['identity']
                    entries['consumer.'+str(item['identity']['slot'])] = fields[7]['identity']
        sides[side] = entries
    roles = set(sides['model']) | set(sides['exec']) | set(named)
    entries = []
    for role in sorted(roles):
        if role not in sides['model'] or role not in sides['exec'] or role not in named:
            # A rolled-back newly allocated handle has no committed counterpart.
            # It is retained in the opcode trace, never mapped into a live object.
            if role not in named and role not in sides['model'] and role not in sides['exec']:
                continue
            raise ValueError('unmatched optional identity role: '+role)
        entries.append({'role': role, 'model': sides['model'][role], 'exec': sides['exec'][role], 'native': named[role]})
    entries.extend(_transient_access_roles(model,execute,native,entries,input_value))
    return entries


def _transient_access_roles(model, execute, native, existing, input_value):
    """Bind only actually issued, rolled-back access capabilities with journal proof."""
    if model.get('ok') or execute.get('ok') or native.get('ok'):
        return []
    operations={60:'beginManagedRead',61:'beginManagedWrite'}
    native_rows=[r for r in native.get('opcodeEvents',[]) if r.get('stage')=='completed' and r.get('opcode') in operations]
    model_ops={f'serviceCall:leanat.managed.{n}':n for n in operations}
    exec_ops={name:n for n,name in operations.items()}
    model_rows=[r for r in model.get('trace',[]) if r.get('operation') in model_ops]
    exec_rows=[r for r in execute.get('trace',[]) if r.get('operation') in exec_ops]
    if len(model_rows)!=len(native_rows) or len(exec_rows)!=len(native_rows):
        raise ValueError('managed completed history is not one-to-one')
    if not native_rows:
        return []
    if input_value is None: raise ValueError('transient access needs authoritative initial input')
    def successful(value):
        return (isinstance(value,dict) and value.get('kind')=='variant' and value.get('tag')==1
                and len(value.get('fields',[]))==1 and value['fields'][0].get('kind')=='handle')
    def interval(row, layer):
        values=[]
        for field in ('fuelBefore','fuelAfter'):
            raw=row.get(field)
            if not isinstance(raw,str) or not raw.isascii() or not raw.isdecimal() or str(int(raw))!=raw:
                raise ValueError('managed history noncanonical fuel interval')
            values.append(int(raw))
        before,after=values
        if before>int(input_value['fuel']) or after>=before:
            raise ValueError('managed history malformed fuel interval')
        if layer!='ModelIR' and before-after!=1:
            raise ValueError('managed VM service fuel cost')
        return before,after
    def model_enter(row):
        index=next(i for i,r in enumerate(model['trace']) if r is row)
        if index==0: raise ValueError('managed source call entry missing')
        entry=model['trace'][index-1]
        if (entry.get('operation')!='enter:leanat.managed.'+str(model_ops[row['operation']]) or
            entry.get('program')!=row.get('program') or entry.get('location')!=row.get('location') or
            entry.get('args')!=row.get('args') or entry.get('results')!=[] or
            entry.get('fuelBefore')!=row.get('fuelBefore') or entry.get('fuelAfter')!=row.get('fuelAfter')):
            raise ValueError('managed source call entry/completion interval differs')
        statements=[r for r in model['trace'][:index-1] if r.get('operation')=='enter:statement' and
                    r.get('program')==row.get('program') and r.get('location')==row.get('location')]
        if (not statements or statements[-1].get('fuelBefore')!=row.get('fuelBefore') or index<2 or
            model['trace'][index-2].get('fuelAfter')!=row.get('fuelAfter')):
            raise ValueError('managed source host interval differs from evaluated argument history')
    def next_generation(counters,h):
        rows=[c for c in counters if c['group']=='managed.access' and
              str(c['domain'])==h['domain'] and str(c['slot'])==h['slot']]
        if len(rows)>1: raise ValueError('duplicate access journal')
        if not rows: return 1
        c=rows[0]
        if not c['persistent'] or c['retired']: raise ValueError('transient access journal policy')
        return int(c['nextGeneration'])
    used={side:{id_key(e[side]) for e in existing} for side in ('model','exec','native')}
    result=[]
    for mr,er,row in zip(model_rows,exec_rows,native_rows):
        key=(str(row['program']),row['source'],row['opcode'])
        if (str(mr['program']),mr['location'],model_ops[mr['operation']])!=key or (str(er['program']),er['location'],exec_ops[er['operation']])!=key:
            raise ValueError('managed completed history order/location differs')
        if mr.get('layer')!='ModelIR' or er.get('layer')!='ExecIR':
            raise ValueError('managed history layer')
        interval(mr,'ModelIR'); interval(er,'ExecIR'); interval(row,'native')
        model_enter(mr)
        binding=IdentityBijection(existing+result)
        arguments=[binding.normalize(mr['args'],'model'),binding.normalize(er['args'],'exec'),
                   binding.normalize(row['arguments'],'native')]
        if arguments[0]!=arguments[1] or arguments[1]!=arguments[2]:
            raise ValueError('managed full call arguments differ')
        if len(mr['results'])!=1 or len(er['results'])!=1: raise ValueError('managed history result arity')
        values={'model':mr['results'][0],'exec':er['results'][0],'native':row['result']}
        if not all(successful(v) for v in values.values()):
            if any(successful(v) for v in values.values()): raise ValueError('managed history admission mismatch')
            normalized=[binding.normalize(values[side],side) for side in ('model','exec','native')]
            if normalized[0]!=normalized[1] or normalized[1]!=normalized[2]: raise ValueError('managed denied result differs')
            continue
        hs={side:identity(value['fields'][0]) for side,value in values.items()}
        if any(id_key(hs[side]) in used[side] for side in hs):
            if not all(id_key(hs[side]) in used[side] for side in hs): raise ValueError('partial managed lifetime correspondence')
            outputs=[binding.normalize(values[side],side) for side in ('model','exec','native')]
            if outputs[0]!=outputs[1] or outputs[1]!=outputs[2]: raise ValueError('managed existing result differs')
            continue
        if hs['model']!=hs['exec']: raise ValueError('reference access history identity differs')
        if hs['model']['store']!='402' or hs['model']['kind']!='9': raise ValueError('noncanonical reference access namespace')
        lease=identity(row['arguments'][0])
        if lease['kind']!='10' or hs['native']['store']!=lease['store']:
            raise ValueError('native access does not use actual manager lease namespace')
        if not any(id_key(item['identity'])==id_key(lease) for item in native.get('initialIdentities',[])):
            raise ValueError('transient access needs independently seeded native lease')
        for field in ('kind','domain','slot','generation','owner'):
            if len({h[field] for h in hs.values()})!=1: raise ValueError('transient access '+field+' differs')
        for side,outcome in [('model',model),('exec',execute)]:
            if any(id_key(o['identity'])==id_key(hs[side]) for o in outcome['world']['objects']):
                raise ValueError('transient access still present in reference state')
        if any(id_key(item['identity'])==id_key(hs['native']) for item in native.get('identities',[])):
            raise ValueError('transient access still present natively')
        initial={'model':input_value['world']['allocationCounters'],'exec':input_value['world']['allocationCounters'],
                 'native':native['initialProvider']['allocation']}
        final={'model':model['world']['allocationCounters'],'exec':execute['world']['allocationCounters'],
               'native':native['provider']['allocation']}
        for side,h in hs.items():
            generation=int(h['generation'])
            if next_generation(initial[side],h)!=generation or next_generation(final[side],h)!=generation+1:
                raise ValueError('transient access lacks exact single allocation burn')
        role='discarded.access.'+str(len(result))
        candidate={'role':role,**hs}
        output_binding=IdentityBijection(existing+result+[candidate])
        outputs=[output_binding.normalize(values[side],side) for side in ('model','exec','native')]
        if outputs[0]!=outputs[1] or outputs[1]!=outputs[2]: raise ValueError('managed full issued result differs')
        result.append(candidate)
        for side,h in hs.items(): used[side].add(id_key(h))
    return result


def _source_tags(outcome, allowed):
    for item in outcome['world']['objects']:
        if item['tag'] not in allowed:
            raise ValueError('unclassified optional object tag: '+item['tag'])
        if not item['alive'] and item['tag'] not in ('storage.result','storage.consumer') and val(item['value']) is not None:
            raise ValueError('unclassified optional tombstone payload')


def _counter(counter):
    if set(counter) != {'group','domain','slot','nextGeneration','persistent','retired'}:
        raise ValueError('allocator counter fields')
    if not isinstance(counter['persistent'],bool) or not isinstance(counter['retired'],bool):
        raise ValueError('allocator counter flags')
    return {'group':counter['group'],'domain':int(counter['domain']),
            'slot':None if counter['slot'] is None else int(counter['slot']),
            'nextGeneration':int(counter['nextGeneration']),
            'persistent':counter['persistent'],'retired':counter['retired']}


def _sort_counters(values):
    return sorted(values,key=lambda c:(c['group'],c['domain'],-1 if c['slot'] is None else c['slot']))


def _policy(policy):
    if set(policy) != {'capacity','perSlot','persistent','allowMax'}:
        raise ValueError('allocator policy fields')
    if any(not isinstance(policy[k],bool) for k in ('perSlot','persistent','allowMax')):
        raise ValueError('allocator policy flags')
    return {**policy,'capacity':int(policy['capacity'])}


def _slot(raw, alive):
    h=identity(raw)
    # Store incarnation remains in raw evidence/role bijection; semantic slot,
    # generation, domain, kind and owner cannot be normalized away.
    return {k:int(h[k]) for k in ('kind','domain','slot','generation','owner')} | {'alive':alive}


def _slots(rows):
    return sorted(rows,key=lambda x:(x['domain'],x['slot']))


def _environment(input_value):
    if input_value is None: raise ValueError("authoritative optional input metadata missing")
    return {item["name"]:item["value"] for item in input_value["context"]["environment"]}


def _managed_limits(input_value):
    if input_value is None: raise ValueError('authoritative optional input metadata missing')
    env=_environment(input_value)
    return val(env['managed.limits']) if 'managed.limits' in env else [128,128,65536]


def source_managed(outcome, input_value=None):
    _source_tags(outcome, {'managed.region','managed.lease','managed.access','managed.scheduler',
                           'storage.result','storage.consumer'})
    if outcome['world']['events'] or outcome['world']['observations']:
        raise ValueError('managed service left unrelated runtime events/observations')
    for region in objects(outcome,'managed.region'):
        h=identity(region['identity'])
        if input_value is None or int(h['domain'])!=int(input_value['context']['domain']) or int(h['owner'])!=int(input_value['context']['owner']):
            raise ValueError('managed region invocation authority')
    regions, leases, accesses, results, consumers, events = [], [], [], [], [], []
    for o in objects(outcome, 'managed.region'):
        a = val(o['value'])
        regions.append([a[0], a[1], a[1]+len(a[8]['bytes'])-1, a[2], a[3], int(a[7]), a[8]])
    for o in objects(outcome, 'managed.lease'):
        a = val(o['value'])
        leases.append([{'kind':'handle','identity':identity(o['identity'])},a[0],a[1],a[1]+a[2]-1,a[3],a[4],a[5]])
    result_values = {id_key(o['identity']): val(o['value']) for o in objects(outcome,'storage.result')}
    for o in objects(outcome, 'managed.access'):
        a=val(o['value'])
        if len(a)!=13: raise ValueError('complete managed access metadata required')
        r=result_values[id_key(a[6])]
        failure,disposition=0,0
        if r[4]:
            failure,disposition=r[5][0],r[5][1]
        accesses.append([{'kind':'handle','identity':identity(o['identity'])},a[0],a[1],a[2],a[3],a[4],a[6],a[7],a[8],a[9],False,a[12],failure,disposition,r[6],r[7],a[8] and not a[9],int(a[5]),a[10],a[11]])
    for o in objects(outcome,'storage.result'):
        a=val(o['value'])
        if len(a) != 12 or a[11] or a[9] != int(o['identity']['owner']):
            raise ValueError('managed result producer ownership/publication state')
        if a[10] != int(o['identity']['owner']):
            raise ValueError('managed initial consumer owner differs from producer')
        results.append([{'kind':'handle','identity':identity(o['identity'])},a[0],a[2],a[3],a[4],a[5],None if not a[4] else [a[6],a[7]],a[8]])
    for o in objects(outcome,'storage.consumer'):
        a=val(o['value'])
        consumers.append([{'kind':'handle','identity':identity(o['identity'])},a[0],a[1]])
    schedulers = objects(outcome, 'managed.scheduler')
    if len(schedulers)>1: raise ValueError('duplicate managed scheduler')
    last_sequence=0
    if schedulers:
        last_sequence, pending = val(schedulers[0]['value'])
        for time, sequence, kind, args in pending:
            events.append([bytes(kind['bytes']).decode('utf-8'), time, sequence]+args)
    events.sort(key=lambda x:(x[1],x[2]))
    generations = []
    for counter in outcome['world']['allocationCounters']:
        if counter['group'] in ('managed.lease', 'managed.access'):
            generations.append([counter['group'], int(counter['slot']), int(counter['nextGeneration'])-1])
    generations.sort()
    allocation=[]
    for counter in outcome['world']['allocationCounters']:
        if counter['group'] in ('managed.lease','managed.access','storage.result','storage.consumer'):
            allocation.append(_counter(counter))
        elif counter['group'] == 'logical.managed.scheduler':
            if counter['persistent'] or counter['retired'] or counter['slot'] is not None:
                raise ValueError('managed scheduler logical allocation policy')
        elif counter['group'].startswith('logical.400.'):
            # Host-installed immutable region identity has no native public handle.
            if not objects(outcome,'managed.region'): raise ValueError('orphan region allocator')
            original=(input_value or {}).get('world',{}).get('allocationCounters',[])
            if counter not in original: raise ValueError('immutable region allocator changed')
        else:
            raise ValueError('unclassified optional allocator '+counter['group'])
    resources=sorted([[val(o['value'])[0],val(o['value'])[4],val(o['value'])[5]]
                      for o in objects(outcome,'managed.region')])
    domain=int(objects(outcome,'managed.region')[0]['identity']['domain'])
    for group in ('storage.result','storage.consumer'):
        if not any(c['group']==group and c['domain']==domain for c in allocation):
            imported=[int(o['identity']['generation']) for o in outcome['world']['objects'] if o['tag']==group and int(o['identity']['domain'])==domain]
            allocation.append({'group':group,'domain':domain,'slot':None,'nextGeneration':max(imported,default=0)+1,'persistent':True,'retired':False})
    lease_limit,access_limit,input_limit=_managed_limits(input_value)
    policies={}
    expected={401:('leases','managed.lease',True,lease_limit),402:('accesses','managed.access',True,access_limit),20:('results','storage.result',False,128),21:('consumers','storage.consumer',False,256)}
    for store,(name,group,per_slot,capacity) in expected.items():
        rules=[r for r in outcome['world']['allocationRules'] if int(r['store'])==store]
        if len(rules)>1: raise ValueError('duplicate managed allocator namespace')
        if rules:
            r=rules[0]
            if r['group']!=group: raise ValueError('managed allocator group')
            capacity=r['capacity']
            policies[name]=_policy({k:r[k] for k in ('capacity','perSlot','persistent','allowMax')})
        else:
            policies[name]={'capacity':capacity,'perSlot':per_slot,'persistent':True,'allowMax':True}
    for rule in outcome['world']['allocationRules']:
        if int(rule['store']) not in expected and rule['group'] not in ('logical.managed.scheduler',) and not rule['group'].startswith('logical.400.'):
            raise ValueError('unclassified managed allocator rule')
    result_slots=_slots([_slot(o['identity'],o['alive']) for o in objects(outcome,'storage.result',False)])
    consumer_slots=_slots([_slot(o['identity'],o['alive']) for o in objects(outcome,'storage.consumer',False)])
    pending_input=sum(len(val(o['value'])[4]['bytes']) for o in objects(outcome,'managed.access') if not val(o['value'])[9])
    return {'regions':regions,'leases':leases,'operations':accesses,'results':results,'consumers':consumers,'events':events,'resultCount':len(results),'pins':sum(r[-1] for r in results),'generations':generations,
            'resources':resources,'lastSequence':last_sequence,'allocation':_sort_counters(allocation),
            'allocationPolicies':policies,'resultSlots':result_slots,'consumerSlots':consumer_slots,
            'pinLimit':int(outcome['world']['maxPins']),'inputByteLimit':input_limit,'pendingInputBytes':pending_input}


def native_managed(native, unmapped):
    p=native['provider']
    allowed={'backing','records','results','consumers','resultCount','pins','next','leaseGenerations','accessGenerations','resources','lastSequence','allocation','allocationPolicies','resultSlots','consumerSlots','pinLimit','inputByteLimit','pendingInputBytes'}
    unmapped += ['native.provider.'+k for k in p if k not in allowed]
    for field in ('resources','lastSequence','allocation','allocationPolicies','resultSlots','consumerSlots','pinLimit','inputByteLimit','pendingInputBytes'):
        if field not in p: unmapped.append('native.managed missing '+field)
    if 'records' not in p:
        raise ValueError('actual ManagedStateSnapshot required')
    for key, fields in [('results',{'identity','source','type','maxBytes','producerAlive','publishing','value','ready','pins','consumers'}),
                        ('consumers',{'identity','result'}),('resultSlots',{'identity','alive'}),
                        ('consumerSlots',{'identity','active','result'})]:
        for row in p.get(key,[]):
            if set(row)!=fields: raise ValueError('native managed '+key+' fields')
    if 'allocationPolicies' in p and set(p['allocationPolicies'])!={'leases','accesses','results','consumers'}:
        raise ValueError('native managed policy groups')
    for row in p.get('consumerSlots',[]):
        if row['active'] and not any(id_key(c['identity'])==id_key(row['identity']) and id_key(c['result'])==id_key(row['result']) for c in p['consumers']):
            raise ValueError('active consumer slot differs from ownership record')
    regions,leases,ops,queued=val(p['records'])
    if len(regions)==1 and val(p['backing'])!=regions[0][6]:
        raise ValueError('native backing disagrees with actual region snapshot')
    accesses=[]
    for o in ops:
        if len(o)!=20: raise ValueError('managed operation snapshot schema')
        accesses.append(o)
    results=[]
    for r in p['results']:
        if int(r['type'])!=25 or r['publishing']:
            unmapped.append('native.managed result type/publication not representable')
        results.append([{'kind':'handle','identity':identity(r['identity'])},{'kind':'handle','identity':identity(r['source'])},int(r['maxBytes']),r['producerAlive'],r['value'] is not None,val(r['value']),None if r['ready'] is None else [int(r['ready']['time']),int(r['ready']['turn'])],int(r['pins'])])
        if int(r['consumers'])!=sum(id_key(c['result'])==id_key(r['identity']) for c in p['consumers']):
            raise ValueError('native result consumer ownership count mismatch')
    consumers=[[{'kind':'handle','identity':identity(c['identity'])},{'kind':'handle','identity':identity(c['result'])},False] for c in p['consumers']]
    events=[]
    for e in queued:
        kind=bytes(e[0]['bytes']).decode('ascii')
        events.append([kind,e[1],e[2]]+e[3:])
    events.sort(key=lambda x:(x[1],x[2]))
    expected_next=None if not events else {'time':str(events[0][1]),'turn':str(events[0][2])}
    if p['next']!=expected_next: raise ValueError('native wakeup differs from actual pending events')
    generations = sorted([[group, slot, int(generation)]
                          for group, key in [('managed.lease','leaseGenerations'), ('managed.access','accessGenerations')]
                          for slot, generation in enumerate(p[key]) if int(generation)])
    return {'regions':regions,'leases':leases,'operations':accesses,'results':results,'consumers':consumers,'events':events,'resultCount':int(p['resultCount']),'pins':int(p['pins']),'generations':generations,
            'resources':sorted([[int(x) for x in row] for row in p.get('resources',[])]),
            'lastSequence':int(p['lastSequence']) if 'lastSequence' in p else None,
            'allocation':_sort_counters([_counter(c) for c in p.get('allocation',[])]),
            'allocationPolicies':{k:_policy(v) for k,v in p.get('allocationPolicies',{}).items()},
            'resultSlots':_slots([_slot(row['identity'],row['alive']) for row in p.get('resultSlots',[]) if int(identity(row['identity'])['generation'])]),
            'consumerSlots':_slots([_slot(row['identity'],row['active']) for row in p.get('consumerSlots',[]) if int(identity(row['identity'])['generation'])]),
            'pinLimit':int(p['pinLimit']) if 'pinLimit' in p else None,
            'inputByteLimit':int(p['inputByteLimit']) if 'inputByteLimit' in p else None,
            'pendingInputBytes':int(p['pendingInputBytes']) if 'pendingInputBytes' in p else None}


def source_raw(outcome, input_value=None):
    _source_tags(outcome, {'raw.region','raw.grant'})
    if outcome['world']['events']: raise ValueError('raw service left runtime event')
    for rule in outcome['world']['allocationRules']:
        if rule['group'] not in ('logical.raw.region','logical.raw.grant') or rule['persistent'] or rule['perSlot'] or not rule['allowMax']:
            raise ValueError('raw internal allocator policy')
    for counter in outcome['world']['allocationCounters']:
        c=_counter(counter)
        if c['group'] not in ('logical.raw.region','logical.raw.grant') or c['persistent'] or c['retired'] or c['slot'] is not None:
            raise ValueError('raw internal allocator journal')
    env=_environment(input_value)
    capacity=val(env['raw.capacity']) if 'raw.capacity' in env else 128
    full_regions=[val(o['value']) for o in objects(outcome,'raw.region')]
    if any(len(r)!=8 or r[7] is None for r in full_regions): raise ValueError('raw backing input not supplied to reference mapping')
    regions=[r[:7] for r in full_regions]
    backing=[r[7] for r in full_regions]
    grants=[val(o['value']) for o in objects(outcome,'raw.grant')]
    observations=[]
    for o in outcome['world']['observations']:
        if o['kind']=='raw.grant':
            a=val(o['values'][0]);observations.append(['raw.grant']+a[1:])
        elif o['kind']=='raw.invalidate':
            observations.append(['raw.invalidate']+[val(x) for x in o['values']])
        else: raise ValueError('unexpected optional raw observation')
    return {'regions':regions,'backing':backing,'grants':grants,'observations':observations,'rawCapacity':capacity}


def native_raw(native,unmapped):
    p=native['provider'];allowed={'backing','region','grants','observations','rawCapacity'}
    unmapped += ['native.provider.'+k for k in p if k not in allowed]
    if 'rawCapacity' not in p: unmapped.append('native.raw missing rawCapacity')
    if 'region' not in p or 'grants' not in p: raise ValueError('complete raw grant snapshot required')
    region=[int(x) for x in p['region']]
    if len(val(p['backing'])['bytes'])!=region[2]-region[1]+1:raise ValueError('raw backing extent mismatch')
    observations=[]
    for o in p['observations']:
        if set(o)!={'kind','region','start','end','permission','readLatency','writeLatency','generation'}:
            raise ValueError('raw observation fields')
        a=[o['kind'],int(o['region']),int(o['start']),int(o['end'])]
        if o['kind']=='raw.grant':a += [int(o[k]) for k in ['permission','readLatency','writeLatency','generation']]
        elif o['kind']!='raw.invalidate':raise ValueError('unknown raw callback')
        observations.append(a)
    return {'regions':[region],'backing':[val(p['backing'])],'grants':[[g[0]]+[int(x) for x in g[1:]] for g in p['grants']],'observations':observations,'rawCapacity':int(p['rawCapacity']) if 'rawCapacity' in p else None}


def error_agreement(model, execute, native):
    """Map only the exact rejection causes implemented by this provider family."""
    reference = model.get('error')
    actual = native.get('error')
    if model.get('ok') or execute.get('ok') or native.get('ok'):
        return None
    if reference != execute.get('error'):
        return None
    if reference == 'optional rollback after actual service' and actual == reference:
        rule = 'The explicit source fail after the service preserves the same failure text.'
    elif isinstance(actual, dict):
        import re
        causes = {
            'ExternalPrecondition': ('external precondition failed', 64, '0'),
            'RawInvalidationRange': ('raw invalidation range', 45, '0'),
            'RawRegionMissing': ('raw region missing', 45, '11'),
            'StaleReferenceHandle': ('managed lease identity', 60, '3'),
            'WrongOwner': ('optional handle owner', 60, '4'),
            'WrongDomain': ('optional handle domain', 60, '5'),
        }
        cause = causes.get(reference)
        if cause is None:
            return None
        message, opcode, code = cause
        if actual.get('code') != code:
            return None
        if not re.fullmatch(re.escape(message) +
                            r' \(program \d+, block \d+, opcode ' + str(opcode) +
                            r', handler/\d+/body/\d+\)', actual.get('message', '')):
            return None
        rule = ('The reference rejection and native error code ' + code + ' identify the same '
                + message + ' check at opcode ' + str(opcode) + '.')
    else:
        return None
    return {'verified': True, 'referenceError': reference, 'nativeError': actual, 'rule': rule}


def project(modelOutcome,execOutcome,nativeOutput,input_value=None):
    unmapped=[]
    p=nativeOutput['provider']
    if 'records' in p:
        entries=role_table(modelOutcome,execOutcome,nativeOutput,input_value)
        model=source_managed(modelOutcome,input_value);execute=source_managed(execOutcome,input_value);native=native_managed(nativeOutput,unmapped)
    elif 'observations' in p:
        entries=[];model=source_raw(modelOutcome,input_value);execute=source_raw(execOutcome,input_value);native=native_raw(nativeOutput,unmapped)
    elif p=={}:
        entries=[]
        def pure(out):
            if out['world']['objects'] or out['world']['events'] or out['world']['observations'] or out['world']['allocationRules'] or out['world']['allocationCounters']:
                raise ValueError('external pure call changed owned state')
            return {}
        model=pure(modelOutcome);execute=pure(execOutcome);native={}
    else:
        raise ValueError('unrecognized optional provider snapshot')
    bijection=IdentityBijection(entries)
    result = {'model':bijection.normalize(model,'model'),'exec':bijection.normalize(execute,'exec'),'native':bijection.normalize(native,'native'),'unmapped':unmapped,'identities':entries}
    agreement = error_agreement(modelOutcome, execOutcome, nativeOutput)
    if agreement is not None:
        result['errorAgreement'] = agreement
    return result

