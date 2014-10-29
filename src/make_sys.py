#!/usr/bin/env python

import numpy as np
import tables 
import sys,os
import cPickle

three_letter_aa = dict(
        A='ALA', C='CYS', D='ASP', E='GLU',
        F='PHE', G='GLY', H='HIS', I='ILE',
        K='LYS', L='LEU', M='MET', N='ASN',
        P='PRO', Q='GLN', R='ARG', S='SER',
        T='THR', V='VAL', W='TRP', Y='TYR')

aa_num = dict([(k,i) for i,k in enumerate(sorted(three_letter_aa.values()))])

one_letter_aa = dict([(v,k) for k,v in three_letter_aa.items()])

deg=np.deg2rad(1)

default_filter = tables.Filters(complib='zlib', complevel=5, fletcher32=True)

def create_array(grp, nm, obj=None):
    return t.create_carray(grp, nm, obj=obj, filters=default_filter)

def write_affine_pair(fasta):
    n_res = len(fasta)
    grp = t.create_group(force, 'affine_pairs')

    ref_pos = np.zeros((n_res,4,3))
    ref_pos[:,0] = (-1.19280531, -0.83127186,  0.)        # N
    ref_pos[:,1] = ( 0.,          0.,          0.)        # CA
    ref_pos[:,2] = ( 1.25222632, -0.87268266,  0.)        # C
    ref_pos[:,3] = ( 0.,          0.94375626,  1.2068012) # CB
    ref_pos[fasta=='GLY',3] = np.nan

    ref_pos -= ref_pos[:,:3].mean(axis=1)[:,None]

    grp._v_attrs.energy_scale = 4.
    grp._v_attrs.dist_cutoff = 6.
    create_array(grp, 'id', obj=np.arange(n_res))
    create_array(grp, 'ref_pos', obj=ref_pos)

def write_affine_alignment(n_res):
    grp = t.create_group(force, 'affine_alignment')

    ref_geom = np.zeros((n_res,3,3))
    ref_geom[:,0] = (-1.19280531, -0.83127186, 0.)  # N
    ref_geom[:,1] = ( 0.,          0.,         0.)  # CA
    ref_geom[:,2] = ( 1.25222632, -0.87268266, 0.)  # C
    ref_geom -= ref_geom.mean(axis=1)[:,None]

    N  = np.arange(n_res)*3 + 0
    CA = np.arange(n_res)*3 + 1
    C  = np.arange(n_res)*3 + 2

    atoms = np.column_stack((N,CA,C))
    create_array(grp, 'atoms', obj=atoms)
    create_array(grp, 'ref_geom', obj=ref_geom)


def write_count_hbond(fasta, hbond_energy, helix_energy_perturbation):
    n_res = len(fasta)
    if hbond_energy > 0.:
        print '\n**** WARNING ****  hydrogen bond formation energy set to repulsive value\n'
    grp = t.create_group(force, 'count_hbond')
    grp._v_attrs.hbond_energy = hbond_energy

    # split into donors and acceptors
    donors = t.create_group(grp, 'donors')
    acceptors = t.create_group(grp, 'acceptors')

    # note that proline is not an hbond donor since it has no NH
    donor_residues    = np.array([i for i in range(n_res) if i>0 and fasta[i]!='PRO'])
    acceptor_residues = np.arange(0,n_res-1)

    print 'hbond, %i donors, %i acceptors in sequence' % (len(donor_residues), len(acceptor_residues))
    
    positive = np.array([x in ('LYS','HIS','ARG') for x in fasta])
    negative = np.array([x in ('ASP','GLU')       for x in fasta])
    electrostatics = 1*positive - 1*negative

    H_bond_length = 0.88
    O_bond_length = 1.24

    create_array(donors,    'bond_length', obj=H_bond_length*np.ones(len(   donor_residues)))
    create_array(acceptors, 'bond_length', obj=O_bond_length*np.ones(len(acceptor_residues)))

    create_array(donors,    'id', obj=np.array((-1,0,1))[None,:] + 3*donor_residues   [:,None])
    create_array(acceptors, 'id', obj=np.array(( 1,2,3))[None,:] + 3*acceptor_residues[:,None])

    create_array(donors,    'residue_id', obj=   donor_residues)
    create_array(acceptors, 'residue_id', obj=acceptor_residues)

    if helix_energy_perturbation is None:
        don_bonus = np.zeros(len(   donor_residues))
        acc_bonus = np.zeros(len(acceptor_residues))
    else:
        import pandas as pd
        bonus = pd.read_csv(helix_energy_perturbation)
        d = dict(zip(df['aa'],zip(df['U_donor'],df['U_acceptor'])))
        don_bonus = np.array([d[fasta[nr]][0] for nr in    donor_residues])
        acc_bonus = np.array([d[fasta[nr]][1] for nr in acceptor_residues])

    create_array(donors,    'helix_energy_bonus', obj=don_bonus)
    create_array(acceptors, 'helix_energy_bonus', obj=acc_bonus)
    return


def make_tab_matrices(phi, theta, bond_length):
    '''TAB matrices are torsion-angle-bond affine transformation matrices'''
    phi         = np.asarray(phi)
    theta       = np.asarray(theta)
    bond_length = np.asarray(bond_length)

    assert phi.shape == theta.shape == bond_length.shape
    r = np.zeros(phi.shape + (4,4), dtype=(phi+theta+bond_length).dtype)
    
    cp = np.cos(phi  ); sp = np.sin(phi  )
    ct = np.cos(theta); st = np.sin(theta)
    l  = bond_length

    r[...,0,0]=   -ct; r[...,0,1]=    -st; r[...,0,2]=   0; r[...,0,3]=   -l*ct;
    r[...,1,0]= cp*st; r[...,1,1]= -cp*ct; r[...,1,2]= -sp; r[...,1,3]= l*cp*st;
    r[...,2,0]= sp*st; r[...,2,1]= -sp*ct; r[...,2,2]=  cp; r[...,2,3]= l*sp*st;
    r[...,3,0]=     0; r[...,3,1]=      0; r[...,3,2]=   0; r[...,3,3]=       1;

    return r


def construct_equilibrium_structure(rama, angles, bond_lengths):
    assert rama.shape == angles.shape == bond_lengths.shape
    n_res = rama.shape[0]
    n_atom = 3*n_res
    assert rama.shape == (n_res,3)

    t = np.zeros(n_atom)
    a = angles.ravel()
    b = bond_lengths.ravel()

    t[3::3] = rama[:-1,1]
    t[4::3] = rama[:-1,2]
    t[5::3] = rama[1: ,0]

    transforms = make_tab_matrices(t,a,b)
    curr_affine = np.eye(4)
    pos = np.zeros((3*n_res,3))

    # right apply all transformations

    for i,mat in enumerate(transforms):
        curr_affine = np.dot(curr_affine, mat)
        pos[i] = curr_affine[:3,3]
    return pos


def random_initial_config(n_res):
    # a reasonable model where the chain grows obeying sensible angles and omegas
    rama    = np.random.random((n_res,3))*2*np.pi - np.pi
    angles  = np.zeros_like(rama)
    lengths = np.zeros_like(rama)

    rama[:,2] = np.pi   # all trans omega's

    angles[:,0] = 120.0*deg  # CA->C->N angle
    angles[:,1] = 120.0*deg  # C->N->CA angle
    angles[:,2] = 109.5*deg  # N->CA->C angle

    lengths[:] = 1.4
    return construct_equilibrium_structure(rama, angles, lengths)


# write dist_spring force
def write_dist_spring(args):
    # create a linear chain
    grp = t.create_group(force, 'dist_spring')
    id = np.arange(n_atom-1)
    id = np.column_stack((id,id+1))

    equil_dist   =  1.4*np.ones(id.shape[0])
    spring_const = args.bond_stiffness*np.ones(id.shape[0])
    bonded_atoms = np.ones(id.shape[0], dtype='bool')

    create_array(grp, 'id', obj=id)
    create_array(grp, 'equil_dist',   obj=equil_dist)
    create_array(grp, 'spring_const', obj=spring_const)
    create_array(grp, 'bonded_atoms', obj=bonded_atoms)

def write_angle_spring(args):
    grp = t.create_group(force, 'angle_spring')
    id = np.arange(n_atom-2)
    id = np.column_stack((id,id+2,id+1))
    equil_angles = np.zeros(id.shape[0])
    equil_angles[0::3] = np.cos(109.5*deg)  # N->CA->C angle
    equil_angles[1::3] = np.cos(120.0*deg)  # CA->C->N angle
    equil_angles[2::3] = np.cos(120.0*deg)  # C->N->CA angle

    create_array(grp, 'id', obj=id)
    create_array(grp, 'equil_dist',   obj=equil_angles)
    create_array(grp, 'spring_const', obj=args.angle_stiffness*np.ones(id.shape[0]))

def write_dihedral_spring():
    # this is primarily used for omega bonds
    grp = t.create_group(force, 'dihedral_spring')
    id = np.arange(1,n_atom-3,3)  # start at CA atom
    id = np.column_stack((id,id+1,id+2,id+3))

    create_array(grp, 'id', obj=id)
    create_array(grp, 'equil_dist',   obj=180*deg*np.ones(id.shape[0]))
    create_array(grp, 'spring_const', obj=30.0*np.ones(id.shape[0]))

# def write_rama_pot():
#     grp = t.create_group(force, 'rama_pot')
#     # first ID is previous C
#     id = np.arange(2,n_atom-4,3)
#     id = np.column_stack((id,id+1,id+2,id+3,id+4))
#     n_bin=72
#     # x_deriv + 1j * y_deriv for convenient packing
#     rama_deriv = np.zeros((id.shape[0],n_bin,n_bin)).astype(np.complex128)
# 
#     d=cPickle.load(open(os.path.join(args.data_dir, 'ubq.rama.pkl')))
#     import scipy.interpolate as interp
#     phi = np.linspace(-np.pi,np.pi,n_bin,endpoint=False) + 2*np.pi/n_bin/2
#     psi = np.linspace(-np.pi,np.pi,n_bin,endpoint=False) + 2*np.pi/n_bin/2
#     def find_deriv(i):
#         rmap = np.tile(d['rama_maps'][i], (3,3))  # tiling helps to ensure periodicity
#         h=d['phi']/180.*np.pi
#         s=d['psi']/180.*np.pi
#         rmap_spline = interp.RectBivariateSpline(
#                 np.concatenate((h-2*np.pi, h, h+2*np.pi)),
#                 np.concatenate((s-2*np.pi, s, s+2*np.pi)),
#                 rmap*1.0)
# 
#         eps = 1e-8
#         dx = (rmap_spline(phi+eps,psi    )-rmap_spline(phi-eps,psi    ))/(2.*eps)
#         dy = (rmap_spline(phi    ,psi+eps)-rmap_spline(phi,    psi-eps))/(2.*eps)
#         return dx+1j*dy
# 
#     for nr in 1+np.arange(rama_deriv.shape[0]):
#         rama_deriv[nr-1] = find_deriv(nr).T
# 
#     idx_to_map = np.arange(id.shape[0])
# 
#     create_array(grp, 'id', obj=id)
#     create_array(grp, 'rama_deriv', obj=rama_deriv)
#     create_array(grp, 'rama_pot',   obj=d['rama_maps'])
#     create_array(grp, 'idx_to_map', obj=idx_to_map)


def basin_cond_prob_fcns(a_phi, a_psi):
    def basin_box(phi0,phi1, psi0,psi1):
        if phi0 > phi1: phi1 += 2*np.pi
        if psi0 > psi1: psi1 += 2*np.pi
        assert phi0 < phi1
        assert psi0 < psi1

        phi_mid  = 0.5*(phi1 + phi0)
        psi_mid  = 0.5*(psi1 + psi0)

        phi_switch = np.cos(phi1 - phi_mid)
        psi_switch = np.cos(psi1 - psi_mid)

        def f(phi,psi, phi_mid=phi_mid, psi_mid=psi_mid, phi_switch=phi_switch, psi_switch=psi_switch,
                a_phi=a_phi, a_psi=a_psi):
            dphi = np.cos(phi - phi_mid)  # cos in the loc function ensures continuous, periodic function
            dpsi = np.cos(psi - psi_mid)

            return 1./(
                 (1.+np.exp(-a_phi*(dphi-phi_switch))) *
                 (1.+np.exp(-a_psi*(dpsi-psi_switch))) )
        return f

    bb = lambda phi0, phi1, psi0,psi1: basin_box(phi0*deg, phi1*deg, psi0*deg, psi1*deg)

    basin_fcns = [
            bb(-180.,   0., -100.,  50.),   # alpha_R
            bb(-180.,-100.,   50.,-100.),   # beta
            bb(-100.,   0.,   50.,-100.),   # PPII
            bb(   0., 180.,  -50., 100.),   # alpha_L
            bb(   0., 180.,  100., -50.)]   # gamma

    basin_cond_prob = [
        (lambda phi,psi, bf=bf: bf(phi,psi)/sum(bf2(phi,psi) for bf2 in basin_fcns))
        for bf in basin_fcns]

    return basin_cond_prob


def approx_maximum_likelihood_fixed_marginal(row_marginal, col_marginal, counts, pseudocount=0.5):
    N = row_marginal.shape[0]
    N = col_marginal.shape[0]

    assert row_marginal.shape == (N,)
    assert col_marginal.shape == (N,)
    assert counts.shape == (N,N)

    row_marginal = row_marginal   / row_marginal .sum()
    col_marginal = col_marginal / col_marginal.sum()

    counts = counts + 1.*pseudocount  # add half pseudocount as in common in such methods
    freq = counts / counts.sum()

    # Equations to be solved are (forall i) sum_j c_{ij}/(lambda_i - lambda_j) == row_marginal_i
    #                            (forall j) sum_i c_{ij}/(lambda_i - lambda_j) == col_marginal_j

    # As an approximation, I will solve (forall i) sum_j c_{ij}/(lambda_i - mean(lambda_j')) == row_marginal_i
    #                                   (forall j) sum_i c_{ij}/(mean(lambda_i') - lambda_j) == col_marginal_j
    # These equations reduce to (forall i) lambda_i - sum_j lambda_j/N == sum_j c_{ij}/row_marginal_i 
    #                           (forall j) sum_j lambda_i/N - lambda_i == sum_i c_{ij}/col_marginal_j
    # and this is a linear system.  It is has many solutions since the LHS is unaffected by the 
    # transformation lambda_i -> lambda_i + alpha, lambda_j -> lambda_j + # alpha.  
    # This is also a symmetry of the exact equations.

    # construct matrix on left-hand-side
    LHS = np.zeros((2*N,2*N));   
    
    LHS[:N,:N] =  np.eye(N);  LHS[:N,N:] =  -1./N; 
    LHS[N:,:N] =   1./N;      LHS[N:,N:] = -np.eye(N)

    RHS = np.zeros((2*N,))
    RHS[:N] = freq.sum(axis=1) / row_marginal
    RHS[N:] = freq.sum(axis=0) / col_marginal

    # the matrix is small, so we can just explicitly invert.  Due to the
    # degeneracy, we will use the pseuodoinverse instead of a regular inverse.

    lambd = np.dot(np.linalg.pinv(LHS), RHS)

    approx_prob = freq / (lambd[:N][:,None] - lambd[N:][None,:])

    return lambd[:N], lambd[N:], approx_prob


def maximum_likelihood_fixed_marginal(row_marginal, col_marginal, counts, pseudocount=0.5):
    N = row_marginal.shape[0]
    N = col_marginal.shape[0]

    assert row_marginal.shape == (N,)
    assert col_marginal.shape == (N,)
    assert counts.shape == (N,N)

    row_marginal = row_marginal   / row_marginal .sum()
    col_marginal = col_marginal / col_marginal.sum()

    lr_approx, lc_approx, prob_approx = approx_maximum_likelihood_fixed_marginal(
            row_marginal, col_marginal, counts, pseudocount)

    counts = counts + 1.*pseudocount  # add half pseudocount as in common in such methods
    freq = counts / counts.sum()

    # see approx_maximum_likelihood_fixed_marginal for discussion of equations to be solved

    def obj(lambd):
        prob = freq / (lambd[:N][:,None] - lambd[N:][None,:])
        print prob.sum()
        val = np.concatenate((
            prob.sum(axis=1) - row_marginal,
            prob.sum(axis=0) - col_marginal))

        # since equations have a symmetry, break it with a condition on the lambda
        val[-1] = lambd[:N].mean() - lambd[N:].mean()
        return val

    import scipy.optimize
    return scipy.optimize.root(obj, np.concatenate((0.5/freq.sum(axis=1), 0.1/freq.sum(axis=0))), method='lm', 
            options=dict(maxiter=100000))


def exact_minimum_chi_square_fixed_marginal(row_marginal, col_marginal, counts, pseudoprob=10.0):
    N = row_marginal.shape[0]
    N = col_marginal.shape[0]

    assert row_marginal.shape == (N,)
    assert col_marginal.shape == (N,)
    assert counts.shape == (N,N)

    row_marginal = row_marginal / row_marginal .sum()
    col_marginal = col_marginal / col_marginal.sum()

    # add pseudocount as in common in such methods
    # this prevents problems with exact zero counts

    counts = counts + pseudoprob * row_marginal[:,None] * col_marginal[None,:]
    freq = counts / counts.sum()

    import cvxopt.base
    matrix = cvxopt.base.matrix

    quadratic_matrix = matrix(np.diag(1./freq.reshape((N*N,))))
    linear_vector = matrix(-2. * np.ones(N*N))

    def marginal_matrix(n, row_or_col):
        x = np.zeros((N,N))
        if row_or_col == 'row':
            x[n,:] = 1.
        elif row_or_col == 'col':
            x[:,n] = 1.
        else:
            raise ValueError
        return x

    # last constraint is redundant, so it is removed
    constraint_matrix = matrix(np.concatenate((
            [marginal_matrix(i, 'row').reshape((N*N)) for i in range(N)],
            [marginal_matrix(j, 'col').reshape((N*N)) for j in range(N)]))[:-1])

    constraint_values = matrix(np.concatenate((row_marginal, col_marginal))[:-1])

    inequality_matrix  = matrix(-np.eye(N*N))  # this is a maximum value constraint
    inequality_cutoffs = matrix( np.zeros(N*N))

    import cvxopt.solvers
    cvxopt.solvers.options['show_progress'] = False
    result = cvxopt.solvers.qp(
            quadratic_matrix,  linear_vector, 
            inequality_matrix, inequality_cutoffs, 
            constraint_matrix, constraint_values)
    assert result['status'] == 'optimal'
    return np.array(result['x']).reshape((N,N))


def minimum_chi_square_fixed_marginal(row_marginal, col_marginal, counts, pseudoprob=10.0, tune_pseudoprob=False):
    # this method is based on Deming and Stephen (1940) 
    # in this method, p_{ij} = n_{ij}/n * (1 + lambda^r_i + lambda^r_j)
    # this equation has a symmetry lambda^r_i += alpha, lambda^c_j -= alpha, so the linear system
    # will be degenerate

    if tune_pseudoprob:
        # in tuning mode, the pseudoprob is increased until all the probability estimates are positive
        # this gives an automated way to ensure that the result is a valid probability distribution
        prob_estimate = minimum_chi_square_fixed_marginal(row_marginal, col_marginal, counts, pseudoprob)
        while np.amin(prob_estimate) < 0.:
            pseudoprob += 1.
            prob_estimate = minimum_chi_square_fixed_marginal(row_marginal, col_marginal, counts, pseudoprob)
        print '%.2f'%(pseudoprob/counts.sum())
        return prob_estimate

    N = row_marginal.shape[0]
    N = col_marginal.shape[0]

    assert row_marginal.shape == (N,)
    assert col_marginal.shape == (N,)
    assert counts.shape == (N,N)

    row_marginal = row_marginal   / row_marginal .sum()
    col_marginal = col_marginal / col_marginal.sum()

    # add pseudocount as in common in such methods
    # this prevents problems with exact zero counts

    counts = counts + pseudoprob * row_marginal[:,None] * col_marginal[None,:]
    freq = counts / counts.sum()
    freq_row_marginal = freq.sum(axis=1)
    freq_col_marginal = freq.sum(axis=0)

    LHS = np.zeros((2*N,2*N))
    LHS[:N,:N] =  np.diag(freq_row_marginal);  LHS[:N,N:] = freq; 
    LHS[N:,:N] =  freq.T;                      LHS[N:,N:] = np.diag(freq_col_marginal);

    RHS = np.zeros((2*N,))
    RHS[:N] = row_marginal - freq_row_marginal
    RHS[N:] = col_marginal - freq_col_marginal

    # The matrix is small, so we can just explicitly invert.  Due to the
    # degeneracy, we will use the pseuodoinverse instead of a regular inverse.

    lambd = np.dot(np.linalg.pinv(LHS), RHS)
    prob = freq * (1. + lambd[:N][:,None] + lambd[N:][None,:])

    # now we have a good estimate for the frequencies, but not perfect, because
    # some of the frequencies could be negative
    return prob


def make_trans_matrices(seq, monomer_basin_prob, dimer_counts):
    N = len(seq)
    trans_matrices = np.zeros((N-1, 5,5))
    prob_matrices  = np.zeros((N-1, 5,5))
    count_matrices = np.zeros((N-1, 5,5))
    assert monomer_basin_prob.shape == (N,5)
    # normalize basin probabilities
    monomer_basin_prob = monomer_basin_prob / monomer_basin_prob.sum(axis=1)[:,None]

    for i in range(N-1):
        count = dimer_counts[(seq[i],seq[i+1])]
        prob = exact_minimum_chi_square_fixed_marginal(
                monomer_basin_prob[i], 
                monomer_basin_prob[i+1], 
                count, pseudoprob = 0.1)   

        # transition matrix is correlation after factoring out independent component
        trans_matrices[i] = prob / (monomer_basin_prob[i][:,None] * monomer_basin_prob[i+1][None,:])
        prob_matrices[i]  = prob
        count_matrices[i] = count

    return trans_matrices, prob_matrices, count_matrices




def populate_rama_maps(seq, rama_library_h5):
    rama_maps = np.zeros((len(seq), 72,72))
    t=tables.openFile(rama_library_h5)
    rama = t.root.rama[:]
    restype = t.root.rama._v_attrs.restype
    dirtype = t.root.rama._v_attrs.dir
    ridx = dict([(x,i) for i,x in enumerate(restype)])
    didx = dict([(x,i) for i,x in enumerate(dirtype)])
    rama_maps[0] = rama[ridx[seq[0]], didx['right'], ridx[seq[1]]]
        
    f = lambda r,d,n: rama[ridx[r], didx[d], ridx[n]]
    for i,l,c,r in zip(range(1,len(seq)-1), seq[:-2], seq[1:-1], seq[2:]):
        rama_maps[i] = f(c,'left',l) + f(c,'right',r) - f(c,'right','ALL')
        
    rama_maps[len(seq)-1] = f(seq[len(seq)-1], 'left', seq[len(seq)-2])
    rama_maps -= -np.log(np.exp(-1.0*rama_maps).sum(axis=-1).sum(axis=-1))[...,None,None]
    t.close()

    return dict(rama_maps = rama_maps, phi=np.arange(-180,180,5), psi=np.arange(-180,180,5))


def write_hmm_pot(sequence, rama_library_h5, dimer_counts=None):
    grp = t.create_group(force, 'hmm_pot')
    # first ID is previous C
    id = np.arange(2,n_atom-4,3)
    id = np.column_stack((id,id+1,id+2,id+3,id+4))
    n_states = 5
    n_bin=72
    rama_deriv = np.zeros((id.shape[0],n_states,n_bin,n_bin,3))

    d=populate_rama_maps(sequence, rama_library_h5)


    import scipy.interpolate as interp
    phi = np.linspace(-np.pi,np.pi,n_bin,endpoint=False) + 2*np.pi/n_bin/2
    psi = np.linspace(-np.pi,np.pi,n_bin,endpoint=False) + 2*np.pi/n_bin/2

    sharpness = 2. ; # parameters set basin sharpness, 4. is fairly diffuse
    basin_cond_prob = basin_cond_prob_fcns(sharpness, sharpness)  
    assert len(basin_cond_prob) == n_states

    def find_deriv(i):
        rmap = np.tile(d['rama_maps'][i], (3,3))  # tiling helps to ensure periodicity
        h=d['phi']/180.*np.pi
        s=d['psi']/180.*np.pi
        rmap_spline = interp.RectBivariateSpline(
                np.concatenate((h-2*np.pi, h, h+2*np.pi)),
                np.concatenate((s-2*np.pi, s, s+2*np.pi)),
                rmap*1.0)

        eps = 1e-8
        vals = []
        for basin in range(n_states):
            # the new axes are to make the broadcasting rules agree
            lprob_fcn = lambda x,y: rmap_spline(x,y) - np.log(basin_cond_prob[basin](x[:,None],y[None,:]))
            p  = np.exp(-lprob_fcn(phi,psi))
            dx = (lprob_fcn(phi+eps,psi    ) - lprob_fcn(phi-eps,psi    ))/(2.*eps)
            dy = (lprob_fcn(phi    ,psi+eps) - lprob_fcn(phi,    psi-eps))/(2.*eps)
            vals.append(np.concatenate((p[...,None],dx[...,None],dy[...,None]), axis=-1))

        # concatenate over basins
        return np.concatenate([x[None] for x in vals], axis=0)

    for nr in 1+np.arange(rama_deriv.shape[0]):
        rama_deriv[nr-1] = find_deriv(nr).transpose((0,2,1,3))

    # P(phi,b) = P(phi) * P(b|phi)
    # P(b|phi) = f(phi, b) / sum_b' f(phi,b)

    # normalize the prob at each site
    rama_deriv[...,0] /= rama_deriv[...,0].sum(axis=1)[:,None]

    idx_to_map = np.arange(id.shape[0])

    if dimer_counts is not None:
        trans_matrices, prob_matrices, count_matrices = make_trans_matrices(
                sequence[1:-1], # exclude termini
                rama_deriv[...,0].sum(axis=-1).sum(axis=-1),
                dimer_counts)
    else:
        # completely uncorrelated transition matrices
        trans_matrices = np.ones((id.shape[0]-1, n_states, n_states))

    grp._v_attrs.sharpness = sharpness
    create_array(grp, 'id',             obj=id)
    create_array(grp, 'rama_deriv',     obj=rama_deriv.astype('f4').transpose((0,1,3,2,4)))
    create_array(grp, 'rama_pot',       obj=d['rama_maps'])
    create_array(grp, 'idx_to_map',     obj=idx_to_map)
    create_array(grp, 'trans_matrices', obj=trans_matrices.astype('f4'))
    if dimer_counts is not None:
        create_array(grp, 'prob_matrices',  obj=prob_matrices)
        create_array(grp, 'count_matrices', obj=count_matrices)


def write_nonbonded(fasta_seq, Vfcns, max_r=10., n_bin=64):
    n_type = len(three_letter_aa)
    assert n_type == 20

    com = t.create_group(force, 'group_com')
    n_group = n_atom/3
    group_inds = -1 + np.zeros((n_group, 3), dtype='i8')

    # groups of 3 to simulate a backbone
    group_inds[:,0] = np.arange(0,n_atom,3)
    group_inds[:,1] = np.arange(1,n_atom,3)
    group_inds[:,2] = np.arange(2,n_atom,3)
    group_type = np.array([aa_num[r] for r in fasta_seq])

    create_array(com, 'group_inds', obj=group_inds)
    create_array(com, 'group_type', obj=group_type)

    pairwise = t.create_group(force, 'pairwise')

    deriv_over_r = np.zeros((n_type, n_type, n_bin), 'f4')
    dx = max_r / (n_bin-1)
    eps = 1e-10
    r = dx*np.arange(n_bin) + eps*1j + 1e-100  # use the complex step numerical differentiation method

    for aa1,i1 in aa_num.items():
        for aa2,i2 in aa_num.items():
            deriv_over_r[i1,i2,1:] = np.imag(Vfcns[aa1,aa2](r[1:])) / eps / np.real(r[1:])
            deriv_over_r[i1,i2, 0] = deriv_over_r[i1,i2, 1]   # kludge for problems at the origin

    create_array(pairwise, 'dist_pot_deriv_over_r', obj=deriv_over_r)
    pairwise.dist_pot_deriv_over_r._v_attrs.dx = dx


def read_fasta(file_obj):
    lines = list(file_obj)
    assert lines[0][0] == '>'
    one_letter_seq = ''.join(x.strip() for x in lines[1:])
    seq = np.array([three_letter_aa[a] for a in one_letter_seq])
    return seq


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Prepare input file')
    parser.add_argument('--fasta', required=True,
            help='[required] FASTA sequence file')
    parser.add_argument('--n-system', type=int, required=True, 
            help='[required] number of systems to prepare')
    parser.add_argument('--output', default='system.h5',
            help='path to output the created .h5 file (default system.h5)')
    parser.add_argument('--residue-radius', type=float, default=1.25,
            help='radius of residue for repulsive interaction (1 kT value)')
    parser.add_argument('--affine', default=False, action='store_true',
            help='use affine nonbonded')
    parser.add_argument('--bond-stiffness', default=48., type=float,
            help='Bond spring constant in units of energy/A^2 (default 48)')
    parser.add_argument('--angle-stiffness', default=175., type=float,
            help='Angle spring constant in units of 1/dot_product (default 175)')
    parser.add_argument('--rama-library', default='',
            help='smooth Rama probability library')
    parser.add_argument('--dimer-basin-library', default='',
            help='dimer basin probability library')
    parser.add_argument('--hbond-energy', default=0., type=float,
            help='energy for forming a hydrogen bond')
    parser.add_argument('--helix-energy-perturbation', default=None,
            help='hbond energy perturbation file for helices')
    parser.add_argument('--initial-structures', default='', 
            help='Pickle file for initial structures for the simulation.  ' +
            'If there are not enough structures for the number of replicas ' +
            'requested, structures will be recycled.  If not provided, a ' +
            'freely-jointed chain with a bond length of 1.4 A will be used ' +
            'instead.')
    args = parser.parse_args()

    fasta_seq = read_fasta(open(args.fasta))

    global n_system, n_atom, t, force
    n_system = args.n_system
    n_atom = 3*len(fasta_seq)
    
    t = tables.openFile(args.output,'w')
    
    input = t.create_group(t.root, 'input')
    create_array(input, 'sequence', obj=fasta_seq)
    
    if args.initial_structures:
        init_pos = cPickle.load(open(args.initial_structures))
        assert init_pos.shape == (n_atom, 3, init_pos.shape[-1])

    pos = np.zeros((n_atom, 3, n_system), dtype='f4')
    for i in range(n_system):
        pos[:,:,i] = init_pos[...,i%init_pos.shape[-1]] if args.initial_structures else random_initial_config(len(fasta_seq))
    create_array(input, 'pos', obj=pos)
    
    force = t.create_group(input,  'force')

    write_dist_spring(args)
    write_angle_spring(args)
    write_dihedral_spring()
    # # write_rama_pot()
    if args.hbond_energy!=0.: write_count_hbond(fasta_seq, args.hbond_energy, args.helix_energy_perturbation)
    dimer_counts = cPickle.load(open(args.dimer_basin_library)) if args.dimer_basin_library else None
    write_hmm_pot(fasta_seq, args.rama_library, dimer_counts=dimer_counts)

    args_group = t.create_group(input, 'args')
    for k,v in sorted(vars(args).items()):
        args_group._v_attrs[k] = v

    if args.residue_radius != 0.:
        height = 20.
        # width = args.residue_radius / np.sqrt(2*np.log(height))
        # V = lambda r: height * np.exp(-0.5 * r**2 / width**2)

        width = 0.3
        radius = args.residue_radius - width * np.log(height-1.)  # set kT energy at desired coordinate
        V = lambda r: height/(1.+np.exp((r-radius)/width))
        # set all atoms to the same function
        Vfcns = dict(((aa1,aa2), V) for aa1 in aa_num for aa2 in aa_num)

        write_nonbonded(fasta_seq, Vfcns)

    if args.affine:
        write_affine_alignment(len(fasta_seq))
        write_affine_pair(fasta_seq)

    t.close()


if __name__ == '__main__':
    main()
