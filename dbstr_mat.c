/*
 * mat.c
 *
 *  Created on: 2016/12/5
 *      Author: Tj
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/********************************************************************************
 * TASK : 1. 修改householder变换函数,使其能存储u向量
 * 		  2. 增加矩阵求逆的函数
 * 		  3. 增加矩阵相乘的函数
 * 		  4. 修改矩阵分解的函数,使其能够适应多维
 *		  5. 修改上三角递推求逆的函数
 *
 * */


#define SGN(x) (x>-0?1:-1)

double* mat(int m,int n){
	double *p=NULL;

	if(m<0||n<0){
		printf("cant be zero!\n");
		return NULL;
	}

	p=(double*)malloc(m*n*sizeof(double));
	if(!p){
		printf("allocate error!\n");
		return NULL;
	}

	memset(p,0,sizeof(double)*m*n);
	return p;
}
/* new integer matrix ----------------------------------------------------------
 * allocate memory of integer matrix
 * args   : int    n,m       I   number of rows and columns of matrix
 * return : matrix pointer (if n<=0 or m<=0, return NULL)
 *-----------------------------------------------------------------------------*/
extern int *imat(int n, int m) {
	int *p;

	if (n <= 0 || m <= 0)
		return NULL;
	if (!(p = (int *) malloc(sizeof(int) * n * m))) {
		printf("allocate memory error!\n");
		return NULL;
	}
	return p;
}
/*
 * input :
 * P for input matrix major in column
 * lda : the actual row length of matrix P
 * W for whole memory space
 * H for half memory space and major in row
 */
void cholesky_LL(double* P, double* L, int lda,int n, char lctl) {
	double* tmp = (double*) mat(n, n);
	int col, k, i;
	for(i=0;i<n;i++)   memcpy(tmp+i*n,P+lda*i,sizeof(double)*n);


	int iptx[n];

	for (i = 0; i < n; i++) {
		iptx[i] = (i + 1) * i / 2;
	}

	for (col = 0; col < n; col++) {
		if (lctl == 'H') {
			L[iptx[col] + col] = sqrt(tmp[col + col * n]);
		} else {
			L[col  + col*n] = sqrt(tmp[col + col * n]);
		}
		for (k = col + 1; k < n; k++) {
			if (lctl == 'H') {
				L[iptx[k] + col] = tmp[k + col * n] / L[iptx[col] + col];
			} else {
				L[k  + col*n] = tmp[k + col * n] / L[col  + col*n];
			}
		}
		for (k = col + 1; k < n; k++) {
			for (i = k; i < n; i++) {
				if (lctl == 'H') {
					tmp[i + k * n] = tmp[i + k * n]
							- L[iptx[i] + col] * L[iptx[k] + col];
				} else {
					tmp[i + k * n] = tmp[i + k * n]
							- L[i  + col*n] * L[k  + col*n];
				}
			}
		}
	}

	free(tmp);
}
/*
 * input :
 * P for input matrix major in column
 * lda : the actual row length of matrix P
 * W for whole memory space
 * H for half memory space and in column mjaor
 */
void cholesky_UU(double *P, double *U, int lda,int n, char uctl) {
	if (n < 0) {
		printf("***cholesky_UU:input arguments cant be negative!\n***");
		return;
	}

	double* tmp = (double*) mat(n, n);
	int col, k, i, iptx[n];
	for(i=0;i<n;i++)   memcpy(tmp+i*n,P+lda*i,sizeof(double)*n);
	for (i = 0; i < n; i++) {
		iptx[i] = (i + 1) * i / 2;
	}
	for (col = n - 1; col >= 0; col--) {
		if (uctl == 'H') {
			U[col + iptx[col]] = sqrt(tmp[col + col * n]);
		} else {
			U[col  + col*n] = sqrt(tmp[col + col*n]);
		}
		for (k = 0; k < col; k++) {
			if (uctl == 'H') {
				U[k + iptx[col]] = tmp[k + col * n] / U[col + iptx[col]];
			} else {
				U[k  + col*n] = tmp[k  + col*n] / U[col  + col*n];
			}
		}
		for (k = 0; k < col; k++) {
			for (i = 0; i < k; i++) {
				if (uctl == 'H') {
					tmp[i + k * n] = tmp[i + k * n]
							- U[i + iptx[col]] * U[k + iptx[col]];
				} else {
					tmp[i  + k*n] = tmp[i  + k*n]
							- U[i  + col*n] * U[k  + col*n];
				}
			}
		}
	}

	free(tmp);
}



//proved to be correct
void matmul(const char* tr, int n, int k, int m, double alpha, const double* A,
		int lda, const double* B, int ldb, double beta, double* C,
		int ldc) {

	double d;
	int i,j,x;
	int opr = tr[0] == 'N' ? (tr[1] == 'N' ? 1 : 2) : (tr[1] == 'N' ? 3 : 4);

	for(i=0;i<n;i++){
		for(j=0;j<m;j++){
			d=0;
			switch(opr){
			case 1:  //NN
				for(x=0;x<k;x++)
					d+=A[x*lda+i]*B[j*ldb+x];
				break;
			case 2:  //NT
				for(x=0;x<k;x++)
					d+=A[x*lda+i]*B[x*ldb+j];
				break;
			case 3:  //TN
				for(x=0;x<k;x++)
					d+=A[i*lda+x]*B[j*ldb+x];
				break;
			case 4:  //TT
				for(x=0;x<k;x++)
					d+=A[i*lda+x]*B[x*ldb+j];
				break;
			}
			if(beta == 0.0)
				C[j*ldc+i]=d*alpha;
			else
				C[j*ldc+i]=d*alpha+beta*C[j*ldc+i];
		}
	}
}

/*
 * input :
 * A          :input matrix major in column
 * lda        : the input matrix column length in the memory
 * index    : for the start of the A column to be converted
 * row      : the row dimension
 * col        : the column to be converted
 * u          : output vector
 * beta     :output beta
 */
int hougenu(double* A,int lda,int index,int row,int col,double* u,double* beta){
	int i;
	double yty=0,sigma=0;
	int iptx = lda*col;
	for(i=index;i<row;i++){
		yty+=A[i+iptx]*A[i+iptx];
	}
	printf("%lf %lf\n",yty,A[index+iptx]*A[index+iptx]);
	if(yty==0 || fabs(yty-A[index+iptx]*A[index+iptx])<1E-10)
		return 0;
	sigma=SGN(A[index+iptx])*sqrt(yty);

	for(i=0;i<row-index;i++){
		if(i==0){
			u[i]=A[i+index+iptx]+sigma;
			A[i+index+iptx]=-1*sigma;
		}else{
			u[i]=A[i+index+iptx];
			A[i+index+iptx]=0;
		}
	}
	*beta=-1.0/(sigma*u[0]);
	printf("the beta is:%lf u[0] is:%lf\n",*beta,u[0]);
	return 1;
}
//prove to be corrected
void housholder(double* A,int lda,int m,int n,int lsav){
	int i,j,col;
	double u[m],beta,sum;
	/*loop for the whole column*/
	for(col=0;col<n;col++){
		/*get the corresponding u vector*/
		memset(u,0,sizeof(double)*m);
		if(hougenu(A,lda,col,m,col,u,&beta)){
			/*save the corresponding u*/
			if(lsav){
				for(i=col;i<m;i++){
					A[lda*col + i + 1]=u[i-col];
				}
				A[lda*col+m+1] =beta;
			}
			/*apply to other column*/
			for(i=col+1;i<n;i++){
				/*compute the ut*y*/
				sum=0.0;
				for(j=col;j<m;j++)
					sum+=u[j-col]*A[j+lda*i];
				for(j=col;j<m;j++)
					A[j+lda*i]+=beta*sum*u[j-col];
			}
		}
	}
}

/*
 * input :
 * U for input matrix column major half memory space
 * lda : the actual column length of matrix U
 * uctl : W for whole space H for half space of the U
 * */
int invrecr_UU(double*U,double* inv,int lda,int n,char uctl){
	int col,row,k;
	if(n<0){
		printf("ndim can't be negative!\n");
		return -1;
	}
	int iptx[n];
	double sum;
	for(col=0;col<n;col++){
		iptx[col]=(col+1)*col /2;
	}
	for(col=0;col<n;col++){
		for(row=0;row<col;row++){
			sum=0.0;
			for(k=row;k<col;k++){
				if(uctl=='H')
					sum+=inv[row+iptx[k]]*U[k+iptx[col]];
				else
					sum+=inv[row+iptx[k]]*U[k+lda*col];
			}
			if(uctl=='H')
				inv[row+iptx[col]]=-sum/U[col+iptx[col]];
			else
				inv[row+iptx[col]]=-sum/U[col+lda*col];
		}
		if(uctl=='H')
			inv[col+iptx[col]]=1.0/U[col+iptx[col]];
		else
			inv[col+iptx[col]]=1.0/U[col+lda*col];
	}
	return 0;
}

/* LU decomposition ----------------------------------------------------------*/
static int ludcmp(double *A, int n, int *indx, double *d) {
	double big, s, tmp, *vv = mat(n, 1);
	int i, imax = 0, j, k;

	*d = 1.0;
	for (i = 0; i < n; i++) {
		big = 0.0;
		for (j = 0; j < n; j++)
			if ((tmp = fabs(A[i + j * n])) > big)
				big = tmp;
		if (big > 0.0)
			vv[i] = 1.0 / big;
		else {
			free(vv);
			return -1;
		}
	}
	for (j = 0; j < n; j++) {
		for (i = 0; i < j; i++) {
			s = A[i + j * n];
			for (k = 0; k < i; k++)
				s -= A[i + k * n] * A[k + j * n];
			A[i + j * n] = s;
		}
		big = 0.0;
		for (i = j; i < n; i++) {
			s = A[i + j * n];
			for (k = 0; k < j; k++)
				s -= A[i + k * n] * A[k + j * n];
			A[i + j * n] = s;
			if ((tmp = vv[i] * fabs(s)) >= big) {
				big = tmp;
				imax = i;
			}
		}
		if (j != imax) {
			for (k = 0; k < n; k++) {
				tmp = A[imax + k * n];
				A[imax + k * n] = A[j + k * n];
				A[j + k * n] = tmp;
			}
			*d = -(*d);
			vv[imax] = vv[j];
		}
		indx[j] = imax;
		if (A[j + j * n] == 0.0) {
			free(vv);
			return -1;
		}
		if (j != n - 1) {
			tmp = 1.0 / A[j + j * n];
			for (i = j + 1; i < n; i++)
				A[i + j * n] *= tmp;
		}
	}
	free(vv);
	return 0;
}
/* LU back-substitution ------------------------------------------------------*/
static void lubksb(const double *A, int n, const int *indx, double *b) {
	double s;
	int i, ii = -1, ip, j;

	for (i = 0; i < n; i++) {
		ip = indx[i];
		s = b[ip];
		b[ip] = b[i];
		if (ii >= 0)
			for (j = ii; j < i; j++)
				s -= A[i + j * n] * b[j];
		else if (s)
			ii = i;
		b[i] = s;
	}
	for (i = n - 1; i >= 0; i--) {
		s = b[i];
		for (j = i + 1; j < n; j++)
			s -= A[i + j * n] * b[j];
		b[i] = s / A[i + i * n];
	}
}
/* inverse of matrix ---------------------------------------------------------*/
extern int inverse(double *A, int lda,int n) {
	double d, *B;
	int i, j, *indx;

	indx = imat(n, 1);
	B = mat(n, n);
	for(i=0;i<n;i++)	memcpy(B+i*n,A+lda*i,sizeof(double)*n);

	if (ludcmp(B, n, indx, &d)) {
		free(indx);
		free(B);
		return -1;
	}
	for (j = 0; j < n; j++) {
		for (i = 0; i < n; i++)
			A[i + j * lda] = 0.0;
		A[j + j * lda] = 1.0;
		lubksb(B, n, indx, A + j * lda);
	}

	free(indx);
	free(B);
	return 0;
}

void print_matrix(double* mat,int row,int col,char* premsg){
	if(row<0||col<0){
		printf("***print_matrix wrong input arguments for the row and col ***");
		return;
	}
	int i,j;
	if(strlen(premsg)!=0){
		printf("%s\n",premsg);
	}
	for(i=0;i<row;i++){
		for(j=0;j<col;j++){
			printf("%12.5lf ",mat[i+j*col]);
		}
		printf("\n");
	}
}
void print_matrix_tri(double* mat,int ndim,char major,char* premsg){
	int iptx[ndim];
	int i,j;
	for(i=0;i<ndim;i++){
		iptx[i]=(i+1)*i/2;
	}
	if(strlen(premsg)!=0)
		printf("%s\n",premsg);
	for(i=0;i<ndim;i++){
		for(j=0;j<ndim;j++){
			switch(major){
			case 'U':
				if(i<=j)
					printf("%12.3lf ",mat[i+iptx[j]]);
				else
					printf("%12.3lf ",0.0);
				break;
			case 'L':
				if(i>=j)
					printf("%12.3lf ",mat[j+iptx[i]]);
				else
					printf("%12.3lf ",0.0);
				break;
			}
		}
		printf("\n");
	}
}



#define ndim 10
int main(int argc,char* args[]){
	int i;
	double* P=(double*)malloc(sizeof(double)*ndim*ndim);
	double* D=(double*)malloc(sizeof(double)*ndim*ndim);
	memset(D,0,sizeof(double)*ndim*ndim);
	memset(P,0,sizeof(double)*ndim*ndim);

	FILE* fp=NULL;
	if(!(fp=fopen("E:\\data.txt","r"))){
		printf("cant open file to read!\n");
		exit(1);
	}
	int index=0;
	char buffer[10240];
	for(index=0;index<ndim;index++){
		fgets(buffer,10240,fp);
		printf("%s\n",buffer);
		for(i=0;i<ndim;i++){
			sscanf(buffer+i*10,"%lf",P+ndim*i+index);
		}
	}
	print_matrix(P,ndim,ndim,"P MATRIX");

	//inverse(P,ndim,7);

	housholder(P,ndim,6,6,1);

	print_matrix(P,ndim,ndim,"P MATRIX");

	free(P);
	free(D);
}








































