"""Negative controls against captured, actually executed Optional provider outcomes."""
import copy
import json
import sys
from pathlib import Path
try:
    from .opcode_projection_optional import project, error_agreement, source_managed, native_managed, role_table
    from .opcode_projection_common import IdentityBijection
except ImportError:
    from opcode_projection_optional import project, error_agreement, source_managed, native_managed, role_table
    from opcode_projection_common import IdentityBijection


def accepted(model, execute, native, input_value=None):
    try:
        p=project(model,execute,native,input_value)
        return not p['unmapped'] and p['model']==p['exec']==p['native']
    except (ValueError,KeyError,TypeError):
        return False


def controls(folder):
    folder=Path(folder)
    load=lambda suffix:json.loads((folder/('optional.read-success.'+suffix+'.json')).read_text(encoding='utf-8-sig'))
    model,execute,native=load('model'),load('exec'),load('c4')
    input_value=load('input')
    assert accepted(model,execute,native,input_value), 'positive actual fixture must agree first'
    mutations=[]
    def owner(n): n['provider']['records']['array'][2]['array'][0]['array'][0]['handle']['owner']='10'
    mutations.append(('owner',owner))
    def generation(n): n['identities'][0]['identity']['generation']=str(int(n['identities'][0]['identity']['generation'])+1)
    mutations.append(('generation',generation))
    def lifetime(n): n['provider']['records']['array'][1]['array']=[]
    mutations.append(('lifetime',lifetime))
    def capacity(n): n['provider']['resultCount']=str(int(n['provider']['resultCount'])+1)
    mutations.append(('capacity-accounting',capacity))
    def bytes_changed(n): n['provider']['backing']['bytes'][0]='255'
    mutations.append(('backing',bytes_changed))
    def publication(n): n['provider']['results'][0]['value']={'u64':'99'}
    mutations.append(('publication',publication))
    def unknown(n): n['provider']['unexplainedNativeSideEffect']=True
    mutations.append(('unmapped-effect',unknown))
    for name,mutation in mutations:
        altered=copy.deepcopy(native);mutation(altered)
        assert not accepted(model,execute,altered,input_value), 'negative control accepted: '+name
    error_controls = 0
    for path in folder.glob('optional.*.model.json'):
        model = json.loads(path.read_text(encoding='utf-8-sig'))
        if model['ok']:
            continue
        stem = path.name.removesuffix('.model.json')
        execute = json.loads((folder/(stem+'.exec.json')).read_text(encoding='utf-8-sig'))
        native = json.loads((folder/(stem+'.c4.json')).read_text(encoding='utf-8-sig'))
        assert error_agreement(model, execute, native), 'unmapped actual failure: '+stem
        altered = copy.deepcopy(native)
        altered['error'] = 'unrelated rejection'
        assert error_agreement(model, execute, altered) is None, 'unrelated failure matched: '+stem
        error_controls += 1
    return len(mutations) + error_controls

def schema_controls():
    """Synthetic projection unit checks; these are not native execution evidence."""
    bits=lambda n:{'kind':'bits','width':64,'value':str(n)}
    ident={'kind':10,'domain':'1','store':'400','slot':'0','generation':'1','owner':'7'}
    region={'identity':ident,'tag':'managed.region','alive':True,'value':{'kind':'record','fields':[
        bits(1),bits(0),bits(3),bits(1),bits(5),bits(7),bits(25),
        {'kind':'bool','value':False},{'kind':'bytes','data':[1,2]}]}}
    outcome={'world':{'objects':[region],'events':[],'observations':[],
        'allocationCounters':[],'allocationRules':[],'maxPins':256}}
    inp={'context':{'environment':[],'domain':'1','owner':'7'}}
    baseline=source_managed(outcome,inp)
    changed=copy.deepcopy(outcome)
    changed['world']['objects'][0]['value']['fields'][5]=bits(999)
    assert source_managed(changed,inp)!=baseline, 'resource frontier lost'
    changed=copy.deepcopy(outcome)
    changed['world']['allocationCounters']=[{'group':'storage.result','domain':'1','slot':None,
        'nextGeneration':'999','persistent':True,'retired':False}]
    assert source_managed(changed,inp)!=baseline, 'result allocator lost'
    changed=copy.deepcopy(outcome)
    changed['world']['allocationRules']=[{'kind':5,'store':20,'group':'storage.result',
        'capacity':1,'perSlot':False,'persistent':True,'allowMax':True}]
    assert source_managed(changed,inp)!=baseline, 'allocator capacity lost'
    for tag in ('unexpected.mutable','reference.event'):
        changed=copy.deepcopy(outcome)
        changed['world']['objects'].append({'identity':ident,'tag':tag,'alive':True,'value':bits(1)})
        try: source_managed(changed,inp)
        except ValueError: pass
        else: raise AssertionError('unknown object accepted: '+tag)
    changed=copy.deepcopy(outcome)
    changed['world']['allocationCounters']=[{'group':'unclassified','domain':'1','slot':None,
        'nextGeneration':'2','persistent':True,'retired':False}]
    try: source_managed(changed,inp)
    except ValueError: pass
    else: raise AssertionError('unknown allocator accepted')
    native={'provider':{'backing':{'bytes':[]},'records':{'array':[{'array':[]} for _ in range(4)]},
        'results':[],'consumers':[],'resultCount':'0','pins':'0','next':None,
        'leaseGenerations':[],'accessGenerations':[]}}
    unmapped=[]
    native_managed(native,unmapped)
    for field in ('resources','lastSequence','allocation','allocationPolicies','resultSlots','consumerSlots','pinLimit','inputByteLimit','pendingInputBytes'):
        assert 'native.managed missing '+field in unmapped, 'missing native field silently accepted: '+field
    policy=lambda n,per_slot:{'capacity':str(n),'perSlot':per_slot,'persistent':True,'allowMax':True}
    native['provider'].update(resources=[['1','5','7']],lastSequence='0',
        allocation=[{'group':'storage.result','domain':'1','slot':None,'nextGeneration':'1','persistent':True,'retired':False}],
        allocationPolicies={'leases':policy(128,True),'accesses':policy(128,True),
                            'results':policy(128,False),'consumers':policy(512,False)},
        resultSlots=[],consumerSlots=[],pinLimit='256',inputByteLimit='65536',pendingInputBytes='0')
    unmapped=[]
    native_baseline=native_managed(native,unmapped)
    assert not unmapped, 'complete synthetic native schema'
    changed=copy.deepcopy(native)
    changed['provider']['resources'][0][2]='999'
    assert native_managed(changed,[])!=native_baseline, 'native frontier mutation lost'
    changed=copy.deepcopy(native)
    changed['provider']['allocation'][0]['nextGeneration']='999'
    assert native_managed(changed,[])!=native_baseline, 'native journal mutation lost'
    changed=copy.deepcopy(native)
    changed['provider']['allocationPolicies']['results']['capacity']='1'
    assert native_managed(changed,[])!=native_baseline, 'native capacity mutation lost'
    changed=copy.deepcopy(native)
    changed['provider']['resultSlots']=[{'identity':{**ident,'kind':5,'store':'20','generation':'9'},'alive':False}]
    assert native_managed(changed,[])!=native_baseline, 'native tombstone lost'
    changed=copy.deepcopy(native)
    changed['provider']['resultSlots']=[{'identity':ident,'alive':False,'unclassified':True}]
    try: native_managed(changed,[])
    except ValueError: pass
    else: raise AssertionError('unknown nested slot field accepted')
    return 12


def transient_controls(model, execute, native, input_value):
    """Check captured rollback history, never fabricate a native issued handle."""
    roles=role_table(model,execute,native,input_value)
    discarded=[r for r in roles if r['role'].startswith('discarded.access.')]
    assert len(discarded)==1, 'one actually issued and rolled-back access'
    role=discarded[0]
    assert role['model']['store']=='402' and role['native']['store']=='401'
    binding=IdentityBijection(roles)
    for side in ('model','exec','native'):
        normalized=binding.normalize({'kind':'handle','identity':role[side]},side)
        assert normalized==binding.normalize({'kind':'handle','identity':role['model']},'model')
    for field in ('store','slot','generation','owner','domain','kind'):
        changed=copy.deepcopy(native)
        row=next(r for r in changed['opcodeEvents'] if r['stage']=='completed' and r['opcode'] in (60,61))
        h=row['result']['fields'][0]['identity']
        h[field]=str(int(h[field])+1)
        try: role_table(model,execute,changed,input_value)
        except ValueError: pass
        else: raise AssertionError('forged transient identity accepted: '+field)
    changed=copy.deepcopy(native)
    next(c for c in changed['provider']['allocation'] if c['group']=='managed.access')['nextGeneration']='1'
    try: role_table(model,execute,changed,input_value)
    except ValueError: pass
    else: raise AssertionError('lost persistent burn accepted')
    changed=copy.deepcopy(native)
    changed['opcodeEvents']=[r for r in changed['opcodeEvents'] if r['opcode'] not in (60,61)]
    try: role_table(model,execute,changed,input_value)
    except ValueError: pass
    else: raise AssertionError('unpaired reference history accepted')
    assert binding.normalize({'kind':'handle','identity':{**role['native'],'generation':'99'}},'native').get('identity')
    operation='serviceCall:leanat.managed.'+str(next(r['opcode'] for r in native['opcodeEvents'] if r['stage']=='completed' and r['opcode'] in (60,61)))
    changed=copy.deepcopy(model)
    call=next(r for r in changed['trace'] if r['operation']==operation)
    changed['trace'].append(copy.deepcopy(call))
    try: role_table(changed,execute,native,input_value)
    except ValueError: pass
    else: raise AssertionError('duplicated Model issuance accepted')
    for index in (0,1,2):
        changed=copy.deepcopy(model)
        call=next(r for r in changed['trace'] if r['operation']==operation)
        # Change both source call entry and completion, so the cross-backend
        # full-argument check must catch it, not only the local interval pair.
        entry=changed['trace'][changed['trace'].index(call)-1]
        call['args'][index]={'kind':'bool','value':False}
        entry['args']=copy.deepcopy(call['args'])
        try: role_table(changed,execute,native,input_value)
        except ValueError: pass
        else: raise AssertionError('changed Model argument accepted: '+str(index))
    for before,after in [('1','2'),('1001','999'),('0998','993'),('998','998'),('997','992')]:
        changed=copy.deepcopy(model)
        call=next(r for r in changed['trace'] if r['operation']==operation)
        entry=changed['trace'][changed['trace'].index(call)-1]
        call['fuelBefore']=entry['fuelBefore']=before
        call['fuelAfter']=entry['fuelAfter']=after
        try: role_table(changed,execute,native,input_value)
        except ValueError: pass
        else: raise AssertionError('malformed Model host interval accepted')
    changed=copy.deepcopy(execute)
    changed['trace'].append(copy.deepcopy(next(r for r in changed['trace'] if r['operation'] in ('beginManagedRead','beginManagedWrite'))))
    try: role_table(model,changed,native,input_value)
    except ValueError: pass
    else: raise AssertionError('duplicated Exec issuance accepted')
    return 19


if __name__=='__main__':
    if len(sys.argv)>2:raise SystemExit('usage: test_opcode_projection_optional.py [captured-case-directory]')
    print('Optional projection schema/mutation unit controls passed:',schema_controls())
    if len(sys.argv)==2:
        print('Optional projection actual fixture negative controls passed:',controls(sys.argv[1]))
