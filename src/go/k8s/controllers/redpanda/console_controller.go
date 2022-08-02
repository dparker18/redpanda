// Copyright 2022 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package redpanda

import (
	"context"
	"errors"
	"fmt"

	"github.com/go-logr/logr"
	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"

	redpandav1alpha1 "github.com/redpanda-data/redpanda/src/go/k8s/apis/redpanda/v1alpha1"
	adminutils "github.com/redpanda-data/redpanda/src/go/k8s/pkg/admin"
	consolepkg "github.com/redpanda-data/redpanda/src/go/k8s/pkg/console"
	"github.com/redpanda-data/redpanda/src/go/k8s/pkg/resources"
)

// ConsoleReconciler reconciles a Console object
type ConsoleReconciler struct {
	client.Client
	Scheme                *runtime.Scheme
	Log                   logr.Logger
	AdminAPIClientFactory adminutils.AdminAPIClientFactory
	clusterDomain         string
}

//+kubebuilder:rbac:groups=apps,resources=deployments,verbs=get;list;watch;create;update;patch;delete

//+kubebuilder:rbac:groups=redpanda.vectorized.io,resources=consoles,verbs=get;list;watch;create;update;patch;delete
//+kubebuilder:rbac:groups=redpanda.vectorized.io,resources=consoles/status,verbs=get;update;patch
//+kubebuilder:rbac:groups=redpanda.vectorized.io,resources=consoles/finalizers,verbs=update

func (r *ConsoleReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	log := r.Log.WithValues("redpandaconsole", req.NamespacedName)

	log.Info(fmt.Sprintf("Starting reconcile loop for %v", req.NamespacedName))
	defer log.Info(fmt.Sprintf("Finished reconcile loop for %v", req.NamespacedName))

	console := &redpandav1alpha1.Console{}
	if err := r.Get(ctx, req.NamespacedName, console); err != nil {
		if apierrors.IsNotFound(err) {
			return ctrl.Result{}, nil
		}
		return ctrl.Result{}, err
	}

	cluster := &redpandav1alpha1.Cluster{}
	if err := r.Get(ctx, console.GetClusterRef(), cluster); err != nil {
		return ctrl.Result{}, err
	}

	var s state
	if console.GetDeletionTimestamp() != nil {
		s = &Deleting{r}
	} else {
		s = &Reconciling{r}
	}

	return s.Do(ctx, console, cluster, log)
}

// Reconciling is the state of the Console that handles reconciliation
type Reconciling ConsoleState

// Do handles reconciliation of Console
func (r *Reconciling) Do(ctx context.Context, console *redpandav1alpha1.Console, cluster *redpandav1alpha1.Cluster, log logr.Logger) (ctrl.Result, error) {
	applyResources := []resources.Resource{
		consolepkg.NewKafkaSA(r.Client, r.Scheme, console, cluster, r.clusterDomain, r.AdminAPIClientFactory, log),
		consolepkg.NewKafkaACL(r.Client, r.Scheme, console, cluster, log),
		consolepkg.NewConfigMap(r.Client, r.Scheme, console, cluster, log),
		consolepkg.NewDeployment(r.Client, r.Scheme, console, cluster, log),
		consolepkg.NewService(r.Client, r.Scheme, console, log),
	}

	for _, each := range applyResources {
		if err := each.Ensure(ctx); err != nil {
			var e *resources.RequeueAfterError
			if errors.As(err, &e) {
				return ctrl.Result{RequeueAfter: e.RequeueAfter}, nil
			}
			return ctrl.Result{}, err
		}
	}

	return ctrl.Result{}, nil
}

// Deleting is the state of the Console that handles deletion
type Deleting ConsoleState

// Do handles deletion of Console
func (r *Deleting) Do(ctx context.Context, console *redpandav1alpha1.Console, cluster *redpandav1alpha1.Cluster, log logr.Logger) (ctrl.Result, error) {
	applyResources := []resources.ManagedResource{
		consolepkg.NewKafkaSA(r.Client, r.Scheme, console, cluster, r.clusterDomain, r.AdminAPIClientFactory, log),
		consolepkg.NewKafkaACL(r.Client, r.Scheme, console, cluster, log),
	}

	for _, each := range applyResources {
		if err := each.Cleanup(ctx); err != nil {
			return ctrl.Result{}, err
		}
	}

	return ctrl.Result{}, nil
}

// SetupWithManager sets up the controller with the Manager.
func (r *ConsoleReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&redpandav1alpha1.Console{}).
		Owns(&corev1.Secret{}).
		Owns(&corev1.ServiceAccount{}).
		Owns(&corev1.ConfigMap{}).
		Owns(&appsv1.Deployment{}).
		Owns(&corev1.Service{}).
		Complete(r)
}

// WithClusterDomain sets the clusterDomain
func (r *ConsoleReconciler) WithClusterDomain(clusterDomain string) *ConsoleReconciler {
	r.clusterDomain = clusterDomain
	return r
}