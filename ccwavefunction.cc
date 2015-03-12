#include "ccwavefunction.h"
#include "globals.h"
#include "hamiltonian.h"
#include <boost/shared_ptr.hpp>
#include <libqt/qt.h>
#include <libciomr/libciomr.h>
#include <cmath>
#include <libpsio/psio.h>

namespace psi {

CCWavefunction::CCWavefunction(boost::shared_ptr<Wavefunction> reference, boost::shared_ptr<Hamiltonian> H, Options &options, boost::shared_ptr<PSIO> psio) : Wavefunction(options, psio)
{
  outfile->Printf("\n");
  outfile->Printf("\t\t\t**************************\n");
  outfile->Printf("\t\t\t*                        *\n");
  outfile->Printf("\t\t\t*         UGA-CC         *\n");
  outfile->Printf("\t\t\t*                        *\n");
  outfile->Printf("\t\t\t**************************\n");
  outfile->Printf("\n");

  if(options.get_str("REFERENCE") != "RHF")
    throw PSIEXCEPTION("Only for use with RHF references determinants.");

  wfn_ = options.get_str("WFN");
  convergence_ = options.get_double("R_CONVERGENCE");
  maxiter_ = options.get_int("MAXITER");
  do_diis_ = options.get_bool("DIIS");
  ooc_ = options.get_bool("OOC");

  outfile->Printf("\tWave function  = %s\n", wfn().c_str());
  outfile->Printf("\tMaxiter        = %d\n", maxiter());
  outfile->Printf("\tConvergence    = %3.1e\n", convergence());
  outfile->Printf("\tDIIS           = %s\n", do_diis() ? "Yes" : "No");
  outfile->Printf("\tOut-of-core    = %s\n", ooc() ? "Yes" : "No");

  set_reference_wavefunction(reference);
  copy(reference);

  int nfrzv = 0;
  no_ = nv_ = 0;
  for(int i=0; i < nirrep_; i++) {
    no_ += doccpi_[i] - frzcpi_[i];
    nv_ += nmopi_[i] - doccpi_[i] - frzvpi_[i];
    nfrzv += frzvpi_[i];
  }
  char ** labels = molecule_->irrep_labels();

  outfile->Printf("\n\tReference Wfn Parameters:\n");
  outfile->Printf("\t---------------------------\n");
  outfile->Printf("\tNumber of irreps        = %d\n", nirrep_);
  outfile->Printf("\tNumber of MOs           = %d\n", nmo_);
  outfile->Printf("\tNumber of active MOs    = %d\n", no_+nv_);
  outfile->Printf("\tNumber of active occ    = %d\n", no_);
  outfile->Printf("\tNumber of active vir    = %d\n", nv_);
  outfile->Printf("\tNumber of frozen occ    = %d\n", nfrzc_);
  outfile->Printf("\tNumber of frozen vir    = %d\n\n", nfrzv);
  outfile->Printf("\tLabel\t# MOs\t# FZDC\t# DOCC\t# VIRT\t# FZVR\n");
  outfile->Printf("\t-----\t-----\t------\t------\t------\t------\n");
  for(int i=0; i < nirrep_; i++) {
      outfile->Printf("\t %s\t   %d\t    %d\t    %d\t    %d\t    %d\n",
              labels[i],nmopi_[i],frzcpi_[i],doccpi_[i],nmopi_[i]-doccpi_[i],frzvpi_[i]);
    }
  outfile->Printf("\n\tNuclear Repulsion Energy    = %20.15f\n", molecule_->nuclear_repulsion_energy());
  outfile->Printf( "\tFrozen Core Energy          = %20.15f\n", efzc_);
  outfile->Printf( "\tTotal SCF Energy (chkpt)    = %20.15f\n", reference_wavefunction_->reference_energy());

  for(int i=0; i < nirrep_; i++) free(labels[i]);
  free(labels);

  H_ = H; // does this copy properly?

  // Prepare energy denominators
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_p();
  double ****ints = H_->ints_p();

  D1_ = block_matrix(no,nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      D1_[i][a] = fock[i][i] - fock[a+no][a+no];

  D2_ = init_4d_array(no,no,nv,nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          D2_[i][j][a][b] = fock[i][i] + fock[j][j] - fock[a+no][a+no] - fock[b+no][b+no];

  t1_ = block_matrix(no,nv);
  t1old_ = block_matrix(no,nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      t1_[i][a] = fock[i][a+no]/D1_[i][a];

  t2_ = init_4d_array(no,no,nv,nv);
  t2old_ = init_4d_array(no,no,nv,nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          t2_[i][j][a][b] = ints[i][j][a+no][b+no]/D2_[i][j][a][b];

  tau_ = init_4d_array(no,no,nv,nv);
  ttau_ = init_4d_array(no,no,nv,nv);

  build_tau();

  Fvv_ = block_matrix(nv,nv);
  Foo_ = block_matrix(no,no);
  Fov_ = block_matrix(no,nv);

  Woooo_ = init_4d_array(no,no,no,no);
  Wovov_ = init_4d_array(no,nv,no,nv);
  Wovvo_ = init_4d_array(no,nv,nv,no);
}

CCWavefunction::~CCWavefunction()
{
  free_block(D1_);
  free_4d_array(D2_, no_, no_, nv_);
  free_block(t1_);
  free_block(t1old_);
  free_4d_array(t2_, no_, no_, nv_);
  free_4d_array(t2old_, no_, no_, nv_);
  free_4d_array(tau_, no_, no_, nv_);
  free_4d_array(ttau_, no_, no_, nv_);
  free_block(Fvv_);
  free_block(Foo_);
  free_block(Fov_);
  free_4d_array(Woooo_, no_, no_, no_);
  free_4d_array(Wovov_, no_, nv_, no_);
  free_4d_array(Wovvo_, no_, nv_, nv_);
}

double CCWavefunction::compute_energy() { return 0.0; }

double CCWavefunction::energy()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_p();
  double ****L = H_->L_p();
  double **t1 = t1_;
  double ****tau = tau_;

  double one_energy=0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      one_energy = 2 * fock[i][a+no] * t1[i][a];

  double two_energy=0;
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          two_energy += tau[i][j][a][b] * L[i][j][a+no][b+no];

  return one_energy + two_energy;
}

void CCWavefunction::build_tau()
{
  int no = no_;
  int nv = nv_;
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          tau_[i][j][a][b] = t2_[i][j][a][b] + t1_[i][a] * t1_[j][b];
          ttau_[i][j][a][b] = t2_[i][j][a][b] + 0.5 * t1_[i][a] * t1_[j][b];
        }
}

void CCWavefunction::amp_save()
{
  double ****t2tmp = t2_;
  t2_ = t2old_;
  t2old_ = t2tmp;

  double **t1tmp = t1_;
  t1_ = t1old_;
  t1old_ = t1tmp;
}

void CCWavefunction::build_F()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_p();
  double ****L = H_->L_p();
  double **t1 = t1_;
  double ****ttau = ttau_;

  for(int a=0; a < nv; a++)
    for(int e=0; e < nv; e++) {
      double value = fock[a+no][e+no];
      for(int m=0; m < no; m++) {
        value -= 0.5*fock[m][e+no]*t1[m][a];
        for(int f=0; f < nv; f++) {
          value += t1[m][f]*L[m][a+no][f+no][e+no];
          for(int n=0; n < no; n++)
            value -= ttau[m][n][a][f]*L[m][n][e+no][f+no];
        }
      }
      Fvv_[a][e] = value;
    }

  for(int m=0; m < no; m++)
    for(int i=0; i < no; i++) {
      double value = fock[m][i];
      for(int e=0; e < nv; e++) {
        value += 0.5*t1[i][e]*fock[m][e+no];
        for(int n=0; n < no; n++) {
          value += t1[n][e]*L[m][n][i][e+no];
          for(int f=0; f < nv; f++)
            value += ttau[i][n][e][f]*L[m][n][e+no][f+no];
        }
      }
      Foo_[m][i] = value;
    }

  for(int m=0; m < no; m++)
    for(int e=0; e < nv; e++) {
      double value = fock[m][e+no];
      for(int n=0; n < no; n++)
        for(int f=0; f < nv; f++)
          value += t1[n][f]*L[m][n][e+no][f+no];
      Fov_[m][e] = value;
    }
}

void CCWavefunction::build_W()
{
  int no = no_;
  int nv = nv_;
  double ****tau = tau_;
  double **t1 = t1old_;
  double ****t2 = t2old_;
  double ****ints = H_->ints_p();
  double ****L = H_->L_p();

  for(int m=0; m < no; m++)
    for(int n=0; n < no; n++)
      for(int i=0; i < no; i++)
        for(int j=0; j < no; j++) {
          double value = ints[m][n][i][j];
          for(int e=0; e < nv; e++) {
            value += t1[j][e]*ints[m][n][i][e+no] +
                     t1[i][e]*ints[m][n][e+no][j];
            for(int f=0; f < nv; f++)
              value += tau[i][j][e][f]*ints[m][n][e+no][f+no];
          }
          Woooo_[m][n][i][j] = value;
        }

  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int j=0; j < no; j++)
        for(int e=0; e < nv; e++) {
          double value = -ints[m][b+no][j][e+no];
          for(int f=0; f < nv; f++)
            value -= t1[j][f]*ints[m][b+no][f+no][e+no];
          for(int n=0; n < no; n++)
            value += t1[n][b]*ints[m][n][j][e+no];
          for(int n=0; n < no; n++) {
            for(int f=0; f < nv; f++)
              value += ints[m][n][f+no][e+no]*
                (0.5*t2[j][n][f][b] + t1[j][f]*t1[n][b]);
          }
          Wovov_[m][b][j][e] = value;
        }

  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int e=0; e < nv; e++)
        for(int j=0; j < no; j++) {
          double value = ints[m][b+no][e+no][j];
          for(int f=0; f < nv; f++)
            value += t1[j][f]*ints[m][b+no][e+no][f+no];
          for(int n=0; n < no; n++)
            value -= t1[n][b]*ints[m][n][e+no][j];
          for(int n=0; n < no; n++)
            for(int f=0; f < nv; f++)
              value -= ints[m][n][e+no][f+no]*
                (0.5*t2[j][n][f][b] + t1[j][f]*t1[n][b]);
          for(int n=0; n < no; n++) {
            for(int f=0; f < nv; f++)
              value += 0.5*L[m][n][e+no][f+no]*t2[n][j][f][b];
          }
          Wovvo_[m][b][e][j] = value;
        }
}

void CCWavefunction::build_t1()
{
  int no = no_;
  int nv = nv_;
  double **t1new = t1_;
  double **t1 = t1old_;
  double **fock = H_->fock_p();
  double ****ints = H_->ints_p();
  double ****L = H_->L_p();
  double **Fae = Fvv_;
  double **Fmi = Foo_;
  double **Fme = Fov_;
  double ****t2 = t2old_;

  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      double value = fock[a+no][i];
      for(int e=0; e < nv; e++)
        value += t1[i][e]*Fae[a][e];
      for(int m=0; m < no; m++)
        value -= t1[m][a]*Fmi[m][i];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          value += (2*t2[i][m][a][e]-t2[i][m][e][a])*Fme[m][e];
      for(int n=0; n < no; n++)
        for(int f=0; f < nv; f++)
          value += t1[n][f]*L[n][a+no][f+no][i];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          for(int f=0; f < nv; f++)
            value += (2*t2[m][i][e][f]-t2[m][i][f][e])*ints[m][a+no][e+no][f+no];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          for(int n=0; n < no; n++)
            value -= t2[m][n][a][e]*L[n][m][e+no][i];
      t1new[i][a] = value;
   }
}

void CCWavefunction::build_t2()
{
  int no = no_;
  int nv = nv_;
  double ****t2new = t2_;
  double ****t2 = t2old_;
  double ****tau = tau_;
  double ****Wmnij = Woooo_;
  double ****Wmbej = Wovvo_;
  double ****Wmbje = Wovov_;
  double **t1 = t1old_;
  double **Fme = Fov_;
  double **Fmi = Foo_;
  double **Fae = Fvv_;
  double ****ints = H_->ints_p();

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          double value = ints[i][j][a+no][b+no];
          for(int e=0; e < nv; e++)
            value += t2[i][j][a][e]*Fae[b][e] + t2[j][i][b][e]*Fae[a][e];
          for(int e=0; e < nv; e++)
            for(int m=0; m < no; m++)
              value -= 0.5*(t2[i][j][a][e]*t1[m][b]*Fme[m][e] +
                      t2[i][j][e][b]*t1[m][a]*Fme[m][e]);
          for(int m=0; m < no; m++)
            value -=  t2[i][m][a][b]*Fmi[m][j] + t2[m][j][a][b]*Fmi[m][i];
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++)
              value -= 0.5*(t2[i][m][a][b]*t1[j][e]*Fme[m][e] +
                      t2[m][j][a][b]*t1[i][e]*Fme[m][e]);
          for(int m=0; m < no; m++)
            for(int n=0; n < no; n++)
              value += tau[m][n][a][b]*Wmnij[m][n][i][j];
          for(int e=0; e < nv; e++)
            for(int f=0; f < nv; f++)
              value += tau[i][j][e][f]*ints[a+no][b+no][e+no][f+no];
          for(int e=0; e < nv; e++)
            value += t1[i][e]*ints[a+no][b+no][e+no][j] +
                     t1[j][e]*ints[b+no][a+no][e+no][i];
          for(int m=0; m < no; m++)
            value -= t1[m][a]*ints[m][b+no][i][j]+t1[m][b]*ints[m][a+no][j][i];
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++) {
              value += (t2[i][m][a][e] - t2[i][m][e][a]) * Wmbej[m][b][e][j];
              value += t2[i][m][a][e] * (Wmbej[m][b][e][j] + Wmbje[m][b][j][e]);
              value += t2[m][j][a][e] * Wmbje[m][b][i][e];
              value += t2[i][m][e][b] * Wmbje[m][a][j][e];
              value += t2[j][m][b][e] * (Wmbej[m][a][e][i] + Wmbje[m][a][i][e]);
              value += (t2[j][m][b][e] - t2[j][m][e][b]) * Wmbej[m][a][e][i];
            }
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++) {
              value -= t1[i][e]*t1[m][a]*ints[m][b+no][e+no][j];
              value -= t1[i][e]*t1[m][b]*ints[m][a+no][j][e+no];
              value -= t1[j][e]*t1[m][a]*ints[m][b+no][i][e+no];
              value -= t1[j][e]*t1[m][b]*ints[m][a+no][e+no][i];
            }
          t2new[i][j][a][b] = value;
        }

  double ****Zmbij = init_4d_array(no, nv, no, no);
  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int i=0; i < no; i++)
        for(int j=0; j < no; j++) {
          Zmbij[m][b][i][j] = 0.0;
          for(int e=0; e < nv; e++)
            for(int f=0; f < nv; f++)
              Zmbij[m][b][i][j] += ints[m][b+no][e+no][f+no] * tau[i][j][e][f];
        }

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          double value = 0.0;
          for(int m=0; m < no; m++)
            value -= t1[m][a]*Zmbij[m][b][i][j];
          t2new[i][j][a][b] += value;
          t2new[j][i][b][a] += value;
        }
  free_4d_array(Zmbij,no,nv,no);
}

double CCWavefunction::t1norm()
{
  int no = no_;
  int nv = nv_;
  double **t1 = t1_;

  double diag = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      diag += t1[i][a] * t1[i][a];

  return sqrt(diag/(2*no));
}

void CCWavefunction::diis(int iter)
{
  int nvector=8;  /* Number of error vectors to keep */
  int word;
  int diis_cycle;
  int vector_length;
  double *error;
  div_t fraction;
  double **B, *C, **vector;
  double product, determinant, maximum;
  psio_address start, end;
  int error_file = 90;
  int amp_file = 91;

  int no = no_; 
  int nv = nv_;

  double **t1 = t1_;
  double **t1old = t1old_;
  double ****t2 = t2_;
  double ****t2old = t2old_;

  /* Calculate the length of a single error vector */
  vector_length = no*nv + no*no*nv*nv;

  /* If we haven't already, open the vector files for reading/writing */
  if(iter == 1) { 
    psio_open(error_file, PSIO_OPEN_NEW);
    psio_open(amp_file, PSIO_OPEN_NEW);
  }

  /* Set the diis cycle value */
  fraction = div((iter-1),nvector);
  diis_cycle = fraction.rem;

  /* Build the current error vector and dump it to disk */
  error = init_array(vector_length);
  word=0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      error[word++] = t1[i][a] - t1old[i][a];
    }

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          error[word++] = t2[i][j][a][b] - t2old[i][j][a][b];
        }

  start = psio_get_address(PSIO_ZERO, diis_cycle*vector_length*sizeof(double));
  psio_write(error_file, "DIIS Error Vectors", (char *) error, 
	     vector_length*sizeof(double), start, &end);

  /* Store the amplitudes, too */
  word=0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      error[word++] = t1[i][a];

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)  {
          error[word++] = t2[i][j][a][b];
        }

  start = psio_get_address(PSIO_ZERO, diis_cycle*vector_length*sizeof(double));
  psio_write(amp_file, "DIIS Amplitude Vectors", (char *) error, 
	     vector_length*sizeof(double), start, &end);
  
  free(error);
    
  /* If we haven't run through enough iterations, set the correct dimensions
     for the extrapolation */
  if(!(iter >= (nvector))) {
    if(iter < 2) return; /* Leave if we can't extrapolate at all */
    nvector = iter;
  }

  /* Now grab the full set of error vectors from the file */
  vector = init_matrix(nvector, vector_length);
  for(int p=0; p < nvector; p++) {
    start = psio_get_address(PSIO_ZERO, p*vector_length*sizeof(double));
    psio_read(error_file, "DIIS Error Vectors", (char *) vector[p], 
	      vector_length*sizeof(double), start, &end);
  }

  /* Build B matrix of error vector products */
  B = init_matrix(nvector+1,nvector+1);

  for(int p=0; p < nvector; p++)
    for(int q=0; q < nvector; q++) {
      dot_arr(vector[p], vector[q], vector_length, &product); 
      B[p][q] = product;
    }

  for(int p=0; p < nvector; p++) {
    B[p][nvector] = -1;
    B[nvector][p] = -1;
  }

  B[nvector][nvector] = 0;

  /* Find the maximum value in B and scale all its elements */
  maximum = fabs(B[0][0]);
  for(int p=0; p < nvector; p++)
    for(int q=0; q < nvector; q++)
      if(fabs(B[p][q]) > maximum) maximum = fabs(B[p][q]);

  for(int p=0; p < nvector; p++)
    for(int q=0; q < nvector; q++)
      B[p][q] /= maximum; 

  /* Build the constant vector */
  C = init_array(nvector+1);
  C[nvector] = -1;

  /* Solve the linear equations */
  flin(B, C, nvector+1, 1, &determinant);

  /* Grab the old amplitude vectors */
  for(int p=0; p < nvector; p++) {
    start = psio_get_address(PSIO_ZERO, p*vector_length*sizeof(double));
    psio_read(amp_file, "DIIS Amplitude Vectors", (char *) vector[p], 
	      vector_length*sizeof(double), start, &end);
  }
  
  /* Build the new amplitude vector from the old ones */
  word=0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      t1[i][a] = 0.0;
      for(int p=0; p < nvector; p++)
        t1[i][a] += C[p]*vector[p][word];
        word++;
    }

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          t2[i][j][a][b] = 0.0;
          for(int p=0; p < nvector; p++)
            t2[i][j][a][b] += C[p]*vector[p][word];
            word++;
        }

  /* Release memory and return */
  free_matrix(vector, nvector);
  free_matrix(B, nvector+1);
  free(C);

  return;
}

double CCWavefunction::increment_amps()
{
  int no = no_;
  int nv = nv_;
  double **D1 = D1_;
  double ****D2 = D2_;
  double **t1 = t1_;
  double **t1old = t1old_;
  double ****t2 = t2_;
  double ****t2old = t2old_;

  double residual1 = 0.0;
  double residual2 = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      residual1 += t1[i][a] * t1[i][a];     
      t1[i][a] = t1old[i][a] + t1[i][a]/D1[i][a];
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++) {
          residual2 += t2[i][j][a][b] * t2[i][j][a][b];
          t2[i][j][a][b] = t2old[i][j][a][b] + t2[i][j][a][b]/D2[i][j][a][b];
        }
    }

  return sqrt(residual1 + residual2);
}

} // psi